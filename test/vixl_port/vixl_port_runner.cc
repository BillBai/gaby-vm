// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
#include "vixl_port_runner.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "embedding_stack.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/simulator.h"

namespace gaby_vm::vixl_port {
namespace {

constexpr uint32_t kRet = 0xd65f03c0u;
constexpr uint64_t kNullLr = 0u;  // RET to null LR terminates the run.

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

// Runs the fixture body+RET on one track and returns the post-run RegisterFile.
// `use_cache` selects RunFrom (cache) vs DebugRunFrom (decoder).
bool RunOneTrack(const PortedFixture& fx,
                 bool use_cache,
                 RegisterFile& out_state,
                 const char* track) {
  // Body words followed by a trailing RET in a contiguous buffer.
  std::vector<uint32_t> buf(fx.code, fx.code + fx.code_words);
  buf.push_back(kRet);
  const uintptr_t entry = reinterpret_cast<uintptr_t>(buf.data());
  const size_t size_bytes = buf.size() * sizeof(uint32_t);

  StackBuffer stack;
  bool ok = true;
  if (use_cache) {
    PredecodeCache cache;
    auto st = cache.RegisterCodeRange(buf.data(), size_bytes);
    if (st != PredecodeCache::RegistrationStatus::Ok) {
      std::fprintf(stderr,
                   "[FAIL] %s / %s track: RegisterCodeRange failed "
                   "(status=%d)\n",
                   fx.name,
                   track,
                   static_cast<int>(st));
      return false;
    }
    Simulator sim(&cache, stack.data(), stack.size());
    SeatEntry(sim, fx, stack);
    sim.RunFrom(entry);
    out_state = sim.ReadAll();
    for (size_t i = 0; i < fx.assert_count; ++i) {
      ok &= CheckAssert(fx, fx.asserts[i], sim, track);
    }
    return ok;
  }
  Simulator sim(nullptr, stack.data(), stack.size());
  SeatEntry(sim, fx, stack);
  sim.DebugRunFrom(entry);
  out_state = sim.ReadAll();
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

bool RunFixture(const PortedFixture& fx) {
  RegisterFile cache_state{}, decoder_state{};
  bool cache_ok = RunOneTrack(fx, /*use_cache=*/true, cache_state, "cache");
  bool decoder_ok =
      RunOneTrack(fx, /*use_cache=*/false, decoder_state, "decoder");
  bool diff_ok = DifferentialEqual(fx, cache_state, decoder_state);
  return cache_ok && decoder_ok && diff_ok;
}

void RunAll(const PortedFixture* fixtures, size_t count, RunStats& stats) {
  for (size_t i = 0; i < count; ++i) {
    stats.cases += 1;
    if (RunFixture(fixtures[i])) {
      stats.passed += 1;
    }
  }
}

}  // namespace gaby_vm::vixl_port
