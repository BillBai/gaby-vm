// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// movprfx_protocol_abort_test: the SVE MOVPRFX protocol, negative case.
//
// A MOVPRFX followed by an instruction that is NOT a legal consumer must abort
// execution via the per-step CanTakeSVEMovprfx protocol check — and it must do
// so on the cache track exactly as it does on the decoder track. This realizes
// the "Illegal MOVPRFX consumer aborts on the cache track" scenario of the
// predecode-cache spec (cache-hotpath-tier1, design D4).
//
// The illegal consumer is a plain scalar `add x0, x0, #1`: its form hash is
// not one of the SVE forms enumerated in Instruction::CanTakeSVEMovprfx, so
// that function's `default` arm returns false, the trailing
// VIXL_CHECK(CanTakeSVEMovprfx(...)) fails, and VIXL aborts (std::abort ->
// SIGABRT). Both RegisterCodeRange and the leaf execution of the scalar add
// succeed — the abort is specifically the MOVPRFX-pair legality check firing
// after the consumer's leaf runs.
//
// This is the tests-first half of the T3 MOVPRFX flag-gating work: it fires
// against the CURRENT per-step form_hash_ implementation and must survive the
// later dispatch change unchanged (the negative test is design D4's guard that
// the check keeps firing after T3 lands).
//
// VIXL_ABORT ultimately calls std::abort(), raising SIGABRT. Following the
// established abort-test convention (typed_register_io_abort_test,
// predecode_cache_data_in_stream_test): fork a child, run the illegal pair in
// the child, waitpid in the parent, and report pass iff the child died from a
// signal. The test program exits 0/1 the normal way, so no CTest WILL_FAIL
// property is needed (and it would not invert a SIGABRT outcome anyway). This
// is why the test is host/CTest-only and excluded from the iOS runner, which
// forbids the fork-and-abort pattern.
//
// Public API only: the instruction words are hand-encoded uint32_t literals,
// verified with an external assembler at authorship time.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

#include "embedding_stack.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/simulator.h"

namespace {

// AArch64 / SVE encodings (VL=128). Verified with an external assembler at
// authorship time.
constexpr uint32_t kMovprfxZ0Z1 = 0x0420bc20u;  // movprfx z0, z1
constexpr uint32_t kAddX0X0_1 = 0x91000400u;    // add     x0, x0, #1  (scalar)
constexpr uint32_t kRet = 0xd65f03c0u;          // ret

// Which track the child drives into the MOVPRFX protocol abort.
enum class Track { kCache, kDecoder };

const char* TrackName(Track track) {
  return track == Track::kCache ? "cache" : "decoder";
}

[[noreturn]] void RunIllegalPairInChild(Track track) {
  // Layout (word offsets):
  //   0: movprfx z0, z1  ; unpredicated constructive prefix
  //   1: add     x0, x0, #1 ; ILLEGAL consumer (plain scalar integer op)
  //   2: ret               ; only reached if the protocol check failed to fire
  alignas(uint32_t) const std::array<uint32_t, 3> code = {{
      kMovprfxZ0Z1,
      kAddX0X0_1,
      kRet,
  }};

  gaby_vm::PredecodeCache cache;
  const gaby_vm::PredecodeCache::RegistrationStatus status =
      cache.RegisterCodeRange(code.data(), code.size() * sizeof(uint32_t));
  if (status != gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    // Registration must succeed (all three words are valid encodings under
    // CPUFeatures::All()); a failure here would make the child exit cleanly
    // and the parent report the test as failed, which is what we want.
    std::fprintf(stderr,
                 "child (%s): RegisterCodeRange did not return Ok before the "
                 "illegal MOVPRFX pair\n",
                 TrackName(track));
    std::_Exit(0);
  }

  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
  sim.Write(gaby_vm::GpRegister::X30, 0);  // LR=0 end-of-sim sentinel.
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code.data());
  if (track == Track::kCache) {
    sim.RunFrom(entry);       // cache track
  } else {
    sim.DebugRunFrom(entry);  // decoder track
  }

  std::fprintf(stderr,
               "child (%s): illegal MOVPRFX consumer ran without aborting\n",
               TrackName(track));
  std::_Exit(0);
}

bool IllegalPairAbortsOnTrack(Track track) {
  // Flush all stdio before forking so the parent's already-buffered output is
  // not inherited (and re-emitted) by the child. This test forks twice, so
  // without the flush the first track's parent-side message would be duplicated
  // through the second child's buffer.
  std::fflush(nullptr);
  const pid_t child = fork();
  if (child < 0) {
    std::perror("fork");
    return false;
  }
  if (child == 0) {
    RunIllegalPairInChild(track);
  }

  int status = 0;
  if (waitpid(child, &status, 0) < 0) {
    std::perror("waitpid");
    return false;
  }

  if (WIFSIGNALED(status)) {
    std::printf(
        "movprfx_protocol_abort: %s-track child died from signal %d (expected "
        "— the CanTakeSVEMovprfx protocol check aborted the illegal pair)\n",
        TrackName(track),
        WTERMSIG(status));
    return true;
  }

  if (WIFEXITED(status)) {
    std::fprintf(stderr,
                 "[FAIL] %s-track child exited normally with status %d — the "
                 "illegal MOVPRFX consumer did not abort\n",
                 TrackName(track),
                 WEXITSTATUS(status));
    return false;
  }

  std::fprintf(stderr,
               "[FAIL] %s-track child terminated in an unexpected way "
               "(status=%d)\n",
               TrackName(track),
               status);
  return false;
}

}  // namespace

int main() {
  int passed = 0;
  int total = 0;

  // Cache track: the scenario under test. Decoder track: the "as it does on
  // the debug track" clause — the abort must be identical across tracks.
  total += 1;
  if (IllegalPairAbortsOnTrack(Track::kCache)) {
    passed += 1;
  }

  total += 1;
  if (IllegalPairAbortsOnTrack(Track::kDecoder)) {
    passed += 1;
  }

  std::printf("movprfx_protocol_abort: %d/%d checks passed\n", passed, total);
  return (passed == total) ? 0 : 1;
}
