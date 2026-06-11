// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
#include "vixl_port_oracle.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace gaby_vm::vixl_port_live {

namespace {
constexpr std::uint64_t kNullLr = 0u;  // LR seeded to the null sentinel.

// X-register code 31 is the stack pointer in a RegisterDump-style result (xzr
// is never a meaningful assert target). Map it to the snapshot's sp; codes
// 0..30 index x[]. Matches the frozen runner using GpRegister(31) == SP.
std::uint64_t SnapshotX(const RegisterFile& got, unsigned code) {
  return (code == 31) ? got.sp : got.x[code];
}

void ReportScalar(const char* name,
                  const char* track,
                  const char* what,
                  unsigned code,
                  std::uint64_t got,
                  std::uint64_t want) {
  std::fprintf(stderr,
               "[FAIL] %s / absolute / %s track: %s%u\n"
               "  actual   = 0x%016" PRIx64
               "\n"
               "  expected = 0x%016" PRIx64 "\n",
               name,
               track,
               what,
               code,
               got,
               want);
}
}  // namespace

std::uint64_t StackTop(void* stack, std::size_t stack_size) {
  auto base = reinterpret_cast<std::uintptr_t>(stack);
  std::uintptr_t top = base + stack_size;
  return static_cast<std::uint64_t>(top & ~std::uintptr_t{15});
}

void SeatEntry(Simulator& sim,
               const RegisterFile& entry,
               void* stack,
               std::size_t stack_size) {
  sim.WriteAll(entry);
  sim.Write(GpRegister::SP, StackTop(stack, stack_size));
  sim.Write(GpRegister::LR, kNullLr);
}

bool CheckX(const RegisterFile& got,
            unsigned code,
            std::uint64_t want,
            const char* track,
            const char* name) {
  std::uint64_t v = SnapshotX(got, code);
  if (v == want) {
    return true;
  }
  ReportScalar(name, track, "X", code, v, want);
  return false;
}

bool CheckW(const RegisterFile& got,
            unsigned code,
            std::uint64_t want,
            const char* track,
            const char* name) {
  std::uint64_t v = SnapshotX(got, code) & 0xffffffffu;
  std::uint64_t w = want & 0xffffffffu;
  if (v == w) {
    return true;
  }
  ReportScalar(name, track, "W", code, v, w);
  return false;
}

bool CheckNzcv(const RegisterFile& got,
               std::uint32_t want,
               const char* track,
               const char* name) {
  if (got.nzcv == want) {
    return true;
  }
  ReportScalar(name, track, "NZCV", 0, got.nzcv, want);
  return false;
}

bool CheckFP16(const RegisterFile& got,
               unsigned code,
               std::uint16_t want_bits,
               const char* track,
               const char* name) {
  std::uint64_t v = got.v[code].lo & 0xffffu;
  if (v == want_bits) {
    return true;
  }
  ReportScalar(name, track, "H", code, v, want_bits);
  return false;
}

bool CheckFP32(const RegisterFile& got,
               unsigned code,
               std::uint32_t want_bits,
               const char* track,
               const char* name) {
  std::uint64_t v = got.v[code].lo & 0xffffffffu;
  if (v == want_bits) {
    return true;
  }
  ReportScalar(name, track, "S", code, v, want_bits);
  return false;
}

bool CheckFP64(const RegisterFile& got,
               unsigned code,
               std::uint64_t want_bits,
               const char* track,
               const char* name) {
  if (got.v[code].lo == want_bits) {
    return true;
  }
  ReportScalar(name, track, "D", code, got.v[code].lo, want_bits);
  return false;
}

bool CheckV128(const RegisterFile& got,
               unsigned code,
               std::uint64_t want_lo,
               std::uint64_t want_hi,
               const char* track,
               const char* name) {
  if (got.v[code].lo == want_lo && got.v[code].hi == want_hi) {
    return true;
  }
  std::fprintf(stderr,
               "[FAIL] %s / absolute / %s track: V%u\n"
               "  actual   = 0x%016" PRIx64 ":%016" PRIx64
               "\n"
               "  expected = 0x%016" PRIx64 ":%016" PRIx64 "\n",
               name,
               track,
               "V",
               code,
               got.v[code].hi,
               got.v[code].lo,
               want_hi,
               want_lo);
  return false;
}

bool DifferentialEqual(const RegisterFile& cache,
                       const RegisterFile& decoder,
                       const char* name) {
  bool ok = true;
  auto diff =
      [&](const char* what, unsigned idx, std::uint64_t x, std::uint64_t y) {
        if (x == y) {
          return;
        }
        std::fprintf(stderr,
                     "[FAIL] %s / differential (cache vs decoder): %s[%u]\n"
                     "  cache   = 0x%016" PRIx64
                     "\n"
                     "  decoder = 0x%016" PRIx64 "\n",
                     name,
                     what,
                     idx,
                     x,
                     y);
        ok = false;
      };
  for (unsigned i = 0; i < 31; ++i) {
    diff("x", i, cache.x[i], decoder.x[i]);
  }
  diff("sp", 0, cache.sp, decoder.sp);
  for (unsigned i = 0; i < 32; ++i) {
    diff("v.lo", i, cache.v[i].lo, decoder.v[i].lo);
    diff("v.hi", i, cache.v[i].hi, decoder.v[i].hi);
  }
  diff("nzcv", 0, cache.nzcv, decoder.nzcv);
  diff("fpcr", 0, cache.fpcr, decoder.fpcr);
  diff("fpsr", 0, cache.fpsr, decoder.fpsr);
  // pc/btype intentionally excluded (pc is the terminal sentinel; btype is a
  // transient BTI tracker not asserted by upstream VIXL).
  return ok;
}

bool EntrySeedingEquivalent(const RegisterFile& reference_reset,
                            const RegisterFile& gaby_readback,
                            const char* context) {
  bool ok = true;
  auto diff =
      [&](const char* what, unsigned idx, std::uint64_t x, std::uint64_t y) {
        if (x == y) {
          return;
        }
        std::fprintf(stderr,
                     "[FAIL] entry-seeding equivalence (%s): %s[%u]\n"
                     "  VIXL ResetState = 0x%016" PRIx64
                     "\n"
                     "  gaby seeded     = 0x%016" PRIx64 "\n",
                     context,
                     what,
                     idx,
                     x,
                     y);
        ok = false;
      };
  // x30/LR and sp are deliberately harness-owned (SeatEntry overrides them), so
  // they are excluded from the equivalence check; everything else must match.
  for (unsigned i = 0; i < 30; ++i) {
    diff("x", i, reference_reset.x[i], gaby_readback.x[i]);
  }
  for (unsigned i = 0; i < 32; ++i) {
    diff("v.lo", i, reference_reset.v[i].lo, gaby_readback.v[i].lo);
    diff("v.hi", i, reference_reset.v[i].hi, gaby_readback.v[i].hi);
  }
  diff("nzcv", 0, reference_reset.nzcv, gaby_readback.nzcv);
  diff("fpcr", 0, reference_reset.fpcr, gaby_readback.fpcr);
  diff("fpsr", 0, reference_reset.fpsr, gaby_readback.fpsr);
  return ok;
}

}  // namespace gaby_vm::vixl_port_live
