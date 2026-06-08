// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
#include "vixl_port_runner.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "embedding_stack.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/simulator.h"

namespace gaby_vm::vixl_port {
namespace {

// `br xzr` — branch to register 31 (the zero register), i.e. to address 0,
// gaby-vm's end-of-simulation sentinel. Used as the trailing terminator instead
// of RET so it works even when the body clobbered LR (e.g. `mov x30, ...`):
// unlike RET it does not read x30, and it clobbers nothing.
constexpr uint32_t kBrXzr = 0xd61f03e0u;
constexpr uint64_t kNullLr = 0u;  // LR seeded to the null sentinel at entry.

// 16-byte-down-aligned top of a StackBuffer, used as the replay SP.
uint64_t StackTop(StackBuffer& stack) {
  auto base = reinterpret_cast<uintptr_t>(stack.data());
  uintptr_t top = base + stack.size();
  return static_cast<uint64_t>(top & ~uintptr_t{15});
}

// Seats entry state on a freshly constructed Simulator: full RegisterFile from
// the fixture, then sp -> this process's stack, LR -> null sentinel.
void SeatEntry(Simulator& sim, const PortedFixture& fx, StackBuffer& stk) {
  sim.WriteAll(fx.entry);
  sim.Write(GpRegister::SP, StackTop(stk));
  sim.Write(GpRegister::LR, kNullLr);
}

bool CheckAssert(const PortedFixture& fx,
                 const AssertTarget& a,
                 Simulator& sim,
                 const char* track) {
  uint64_t got_lo = 0, got_hi = 0, want_lo = a.expected_lo, want_hi = 0;
  const char* what = "?";
  switch (a.kind) {
    case AssertKind::kX:
      got_lo = sim.Read(static_cast<GpRegister>(a.reg));
      what = "X";
      break;
    case AssertKind::kW:
      got_lo = sim.Read(static_cast<GpRegister>(a.reg)) & 0xffffffffu;
      want_lo = a.expected_lo & 0xffffffffu;
      what = "W";
      break;
    case AssertKind::kNZCV:
      got_lo = sim.Read(SysRegister::NZCV);
      what = "NZCV";
      break;
    case AssertKind::kFP32: {
      VRegisterValue v = sim.Read(static_cast<VRegister>(a.reg));
      got_lo = v.lo & 0xffffffffu;
      want_lo = a.expected_lo & 0xffffffffu;
      what = "S";
      break;
    }
    case AssertKind::kFP64: {
      VRegisterValue v = sim.Read(static_cast<VRegister>(a.reg));
      got_lo = v.lo;
      what = "D";
      break;
    }
    case AssertKind::kV128: {
      VRegisterValue v = sim.Read(static_cast<VRegister>(a.reg));
      got_lo = v.lo;
      got_hi = v.hi;
      want_hi = a.expected_hi;
      what = "V";
      break;
    }
  }
  if (got_lo == want_lo && got_hi == want_hi) {
    return true;
  }
  std::fprintf(stderr,
               "[FAIL] %s / absolute / %s track: %s%u\n"
               "  actual   = 0x%016" PRIx64 ":%016" PRIx64
               "\n"
               "  expected = 0x%016" PRIx64 ":%016" PRIx64 "\n",
               fx.name,
               track,
               what,
               a.reg,
               got_hi,
               got_lo,
               want_hi,
               want_lo);
  return false;
}

// Seats `fx`'s entry state on an already-constructed Simulator, runs the body
// from `entry` to termination on one track, and returns the post-run state plus
// the absolute-oracle verdict. The Simulator is reused across fixtures (see
// RunAll): constructing one is ~100ms in a debug build, so amortising it over a
// whole family turns minutes of setup into milliseconds. Both tracks must share
// ONE stack so the seeded sp matches — otherwise the differential oracle would
// report a spurious sp divergence.
//
// RunFrom is unbounded; a non-terminating body would hang. That cannot happen
// for the committed fixtures (the extraction tool excludes every instruction
// family whose gaby behaviour is not load-address-stable), and a *future*
// regression that broke termination is caught by the CTest TIMEOUT on this test
// (see CMakeLists.txt) — a reported failure, not a silent CI hang. That
// backstop is far cheaper than per-instruction StepOnce capping (~20x slower
// here).
bool RunTrackOnSim(Simulator& sim,
                   uintptr_t entry,
                   const PortedFixture& fx,
                   StackBuffer& stack,
                   RegisterFile& out_state,
                   const char* track,
                   bool use_cache) {
  SeatEntry(sim, fx, stack);
  if (use_cache) {
    sim.RunFrom(entry);
  } else {
    sim.DebugRunFrom(entry);
  }
  out_state = sim.ReadAll();
  bool ok = true;
  for (size_t i = 0; i < fx.assert_count; ++i) {
    ok &= CheckAssert(fx, fx.asserts[i], sim, track);
  }
  return ok;
}

// Differential oracle: compares the two tracks' full RegisterFile.
bool DifferentialEqual(const PortedFixture& fx,
                       const RegisterFile& a,
                       const RegisterFile& b) {
  bool ok = true;
  auto diff = [&](const char* name, unsigned idx, uint64_t x, uint64_t y) {
    if (x == y) {
      return;
    }
    std::fprintf(stderr,
                 "[FAIL] %s / differential (cache vs decoder): %s[%u]\n"
                 "  cache   = 0x%016" PRIx64
                 "\n"
                 "  decoder = 0x%016" PRIx64 "\n",
                 fx.name,
                 name,
                 idx,
                 x,
                 y);
    ok = false;
  };
  for (unsigned i = 0; i < 31; ++i) {
    diff("x", i, a.x[i], b.x[i]);
  }
  diff("sp", 0, a.sp, b.sp);
  for (unsigned i = 0; i < 32; ++i) {
    diff("v.lo", i, a.v[i].lo, b.v[i].lo);
    diff("v.hi", i, a.v[i].hi, b.v[i].hi);
  }
  diff("nzcv", 0, a.nzcv, b.nzcv);
  diff("fpcr", 0, a.fpcr, b.fpcr);
  diff("fpsr", 0, a.fpsr, b.fpsr);
  // pc/btype intentionally excluded: pc is the terminal sentinel; btype is a
  // transient BTI tracker not asserted by upstream VIXL.
  return ok;
}

}  // namespace

namespace {

// Emit the optional per-fixture trace (VIXL_PORT_TRACE=1), flushed so it
// survives an abort mid-fixture — the way a crash/hang is located to its case.
void TraceFixture(const PortedFixture& fx) {
  if (std::getenv("VIXL_PORT_TRACE") != nullptr) {
    std::fprintf(stderr, "[fixture] %s\n", fx.name);
    std::fflush(stderr);
  }
}

}  // namespace

void RunAll(const PortedFixture* fixtures, size_t count, RunStats& stats) {
  // Build every fixture's body buffer up front (kept alive for the cache's
  // lifetime) and register them all in ONE PredecodeCache, then reuse two
  // Simulators (and one shared stack) across the whole family. Constructing a
  // Simulator is ~100ms in a debug build; amortising it this way turns minutes
  // of setup into milliseconds and scales to the larger FP/NEON families.
  std::vector<std::vector<uint32_t>> bufs;
  bufs.reserve(count);
  PredecodeCache cache;
  std::vector<bool> registered(count, false);
  for (size_t i = 0; i < count; ++i) {
    bufs.emplace_back(fixtures[i].code,
                      fixtures[i].code + fixtures[i].code_words);
    bufs[i].push_back(kBrXzr);
    registered[i] =
        cache.RegisterCodeRange(bufs[i].data(),
                                bufs[i].size() * sizeof(uint32_t)) ==
        PredecodeCache::RegistrationStatus::Ok;
  }

  StackBuffer stack;
  Simulator cache_sim(&cache, stack.data(), stack.size());
  Simulator decoder_sim(nullptr, stack.data(), stack.size());

  for (size_t i = 0; i < count; ++i) {
    const PortedFixture& fx = fixtures[i];
    stats.cases += 1;
    TraceFixture(fx);
    if (!registered[i]) {
      std::fprintf(stderr,
                   "[FAIL] %s / cache track: RegisterCodeRange failed\n",
                   fx.name);
      continue;
    }
    const uintptr_t entry = reinterpret_cast<uintptr_t>(bufs[i].data());
    RegisterFile cache_state{}, decoder_state{};
    bool cache_ok =
        RunTrackOnSim(cache_sim, entry, fx, stack, cache_state, "cache", true);
    bool decoder_ok = RunTrackOnSim(decoder_sim,
                                    entry,
                                    fx,
                                    stack,
                                    decoder_state,
                                    "decoder",
                                    false);
    if (cache_ok && decoder_ok &&
        DifferentialEqual(fx, cache_state, decoder_state)) {
      stats.passed += 1;
    }
  }
}

}  // namespace gaby_vm::vixl_port
