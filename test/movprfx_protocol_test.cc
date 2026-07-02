// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// movprfx_protocol_test: the SVE MOVPRFX protocol, positive case.
//
// A MOVPRFX prefixes a legal destructive consumer; the pair must execute on
// BOTH tracks (cache track via RunFrom, decoder track via DebugRunFrom)
// without tripping the per-step CanTakeSVEMovprfx protocol check, and the
// resulting architectural state must be identical across the two tracks. This
// realizes the "Legal MOVPRFX pair executes identically on both tracks"
// scenario of the predecode-cache spec (cache-hotpath-tier1, design D4).
//
// This is the tests-first half of the T3 MOVPRFX flag-gating work: it passes
// against the CURRENT per-step form_hash_ implementation and must survive the
// later dispatch change unchanged.
//
// The vector length is pinned to 128 in the simulator
// (SetVectorLengthInBits(kZRegMinSize)), so Z0 is exactly the 128-bit V0 and
// the public Read(VRegister::V0) accessor observes the full prefixed
// destination. CPU features are configured to CPUFeatures::All() on the one
// imported Simulator that backs both tracks (gaby_vm/simulator.cc), so SVE is
// available on the decoder track's end-of-step availability check and the
// cache track's predecode auditor alike — the MOVPRFX pair registers and runs
// on both without a feature-availability abort.
//
// Public API only: the instruction words are hand-encoded uint32_t literals,
// verified with an external assembler at authorship time. The build and
// runtime pull in no assembler.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstdio>

#include "embedding_stack.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

namespace {

// AArch64 / SVE encodings (VL=128). Verified with an external assembler at
// authorship time; the assembler also independently confirms the pair is a
// legal MOVPRFX sequence (a predicated MOVPRFX before this ADD, or a scalar
// consumer, is rejected at assembly time — see movprfx_protocol_abort_test).
constexpr uint32_t kMovprfxZ0Z1 = 0x0420bc20u;   // movprfx z0, z1
constexpr uint32_t kAddZ0DZ0D_1 = 0x25e0c020u;   // add     z0.d, z0.d, #1
constexpr uint32_t kRet = 0xd65f03c0u;           // ret            (ret x30)

// Distinctive per-lane source seeded into Z1 (== V1 at VL=128). MOVPRFX copies
// Z1 into Z0 across the whole vector, then ADD adds 1 to each 64-bit (.d) lane.
constexpr uint64_t kZ1Lane0 = 0x1111111111111111ull;  // bits [63:0]
constexpr uint64_t kZ1Lane1 = 0x2222222222222222ull;  // bits [127:64]
constexpr uint64_t kExpectedLane0 = kZ1Lane0 + 1;     // 0x1111111111111112
constexpr uint64_t kExpectedLane1 = kZ1Lane1 + 1;     // 0x2222222222222223

int g_passed = 0;
int g_total = 0;

void check(bool ok, const char* label) {
  g_total += 1;
  if (ok) {
    g_passed += 1;
  } else {
    std::fprintf(stderr, "[FAIL] %s\n", label);
  }
}

// Run the MOVPRFX + legal-consumer pair once and return the prefixed
// destination Z0 (== V0 at VL=128). `use_cache_track` selects RunFrom (cache
// track) vs DebugRunFrom (decoder track); both drive the same code range.
gaby_vm::VRegisterValue run_pair(gaby_vm::PredecodeCache& cache,
                                 const uint32_t* code,
                                 bool use_cache_track) {
  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  // LR=0 convention: the trailing RET reads X30, and 0 is the end-of-sim
  // sentinel that terminates the run. A fresh Simulator already zeroes X30;
  // set it explicitly to document the contract.
  sim.Write(gaby_vm::GpRegister::X30, 0);
  sim.Write(gaby_vm::VRegister::V1,
            gaby_vm::VRegisterValue{kZ1Lane0, kZ1Lane1});

  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);
  if (use_cache_track) {
    sim.RunFrom(entry);
  } else {
    sim.DebugRunFrom(entry);
  }
  return sim.Read(gaby_vm::VRegister::V0);
}

void test_legal_movprfx_pair_runs_on_both_tracks() {
  // Layout (word offsets):
  //   0: movprfx z0, z1        ; unpredicated constructive prefix
  //   1: add     z0.d, z0.d, #1 ; legal destructive consumer (add_z_zi)
  //   2: ret
  alignas(uint32_t) const std::array<uint32_t, 3> code = {{
      kMovprfxZ0Z1,
      kAddZ0DZ0D_1,
      kRet,
  }};

  gaby_vm::PredecodeCache cache;
  check(cache.RegisterCodeRange(code.data(), code.size() * sizeof(uint32_t)) ==
            gaby_vm::PredecodeCache::RegistrationStatus::Ok,
        "MOVPRFX + legal consumer range registers Ok");

  // Reaching the line after each Run means that track executed the pair
  // without the CanTakeSVEMovprfx VIXL_CHECK aborting the process.
  const gaby_vm::VRegisterValue cache_z0 =
      run_pair(cache, code.data(), /*use_cache_track=*/true);
  check(true, "cache-track RunFrom(MOVPRFX pair) executes without aborting");

  const gaby_vm::VRegisterValue debug_z0 =
      run_pair(cache, code.data(), /*use_cache_track=*/false);
  check(true, "decoder-track DebugRunFrom(MOVPRFX pair) executes without aborting");

  // Architectural state is identical across tracks (the spec's core AND).
  check(cache_z0.lo == debug_z0.lo && cache_z0.hi == debug_z0.hi,
        "Z0 identical across cache and decoder tracks");

  // And the pair actually did the MOVPRFX+ADD work (guards against both tracks
  // silently no-op'ing to zero).
  check(cache_z0.lo == kExpectedLane0 && cache_z0.hi == kExpectedLane1,
        "Z0 == (Z1 + 1 per .d lane) on the cache track");
  check(debug_z0.lo == kExpectedLane0 && debug_z0.hi == kExpectedLane1,
        "Z0 == (Z1 + 1 per .d lane) on the decoder track");
}

}  // namespace

int main() {
  test_legal_movprfx_pair_runs_on_both_tracks();

  std::printf("movprfx_protocol: %d/%d checks passed\n", g_passed, g_total);
  return (g_passed == g_total) ? 0 : 1;
}
