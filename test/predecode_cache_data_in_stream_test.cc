// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// predecode_cache_data_in_stream_test: embedded literal data in a code range.
//
// AArch64 code can interleave literal-pool data with real instructions. The
// cache registration path must tolerate data words that decode as unallocated,
// while cache-track execution must still abort if control reaches such a word.
//
// Public API only: the instruction words below are hand-encoded uint32_t
// literals, matching the style of simulator_correctness.cc.
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

constexpr uint32_t kNop = 0xd503201f;  // nop
constexpr uint32_t kRet = 0xd65f03c0;  // ret

// Verified by the current predecode path: before this change, this word makes
// RegisterCodeRange fail with "range contains an unallocated instruction
// encoding". It also gives the literal-pool value distinctive low bytes.
constexpr uint32_t kUnallocatedDataWord = 0xabcd0000;
constexpr uint32_t kDataHighWord = 0x11223344;
constexpr uint64_t kExpectedLiteral =
    (static_cast<uint64_t>(kDataHighWord) << 32) | kUnallocatedDataWord;

const char* StatusName(gaby_vm::PredecodeCache::RegistrationStatus status) {
  using Status = gaby_vm::PredecodeCache::RegistrationStatus;
  switch (status) {
    case Status::Ok:
      return "Ok";
    case Status::InvalidArgument:
      return "InvalidArgument";
    case Status::OverlappingRange:
      return "OverlappingRange";
    case Status::UnsupportedFeature:
      return "UnsupportedFeature";
    case Status::OutOfMemory:
      return "OutOfMemory";
  }
  return "<unknown>";
}

bool ExpectEqU64(uint64_t actual, uint64_t expected, const char* label) {
  if (actual == expected) {
    return true;
  }
  std::fprintf(stderr,
               "[FAIL] %s\n"
               "  actual   = 0x%016llx\n"
               "  expected = 0x%016llx\n",
               label,
               static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(expected));
  return false;
}

bool ExpectRegisterOk(gaby_vm::PredecodeCache& cache,
                      gaby_vm::PredecodeCache::RegistrationStatus status,
                      const char* context) {
  if (status == gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    return true;
  }
  const gaby_vm::PredecodeCache::RegistrationError* error =
      cache.GetLastRegistrationError();
  std::fprintf(stderr,
               "[FAIL] %s: RegisterCodeRange returned %s\n"
               "  reason = %s\n",
               context,
               StatusName(status),
               error != nullptr ? error->reason : "<no error detail>");
  return false;
}

bool RunLiteralPoolLoadOnBothTracks() {
  // Layout (word offsets):
  //   0: nop
  //   1: b +12              ; jump over the two data words to word 4
  //   2: data low word      ; unallocated encoding, non-executable sentinel
  //   3: data high word     ; read as bytes by the LDR literal
  //   4: ldr x0, #-8        ; PC-relative load from word 2
  //   5: ret
  alignas(8) const std::array<uint32_t, 6> code = {{
      kNop,
      0x14000003,  // b #+12
      kUnallocatedDataWord,
      kDataHighWord,
      0x58ffffc0,  // ldr x0, #-8
      kRet,
  }};
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code.data());

  gaby_vm::PredecodeCache cache;
  const gaby_vm::PredecodeCache::RegistrationStatus status =
      cache.RegisterCodeRange(code.data(), code.size() * sizeof(uint32_t));
  if (!ExpectRegisterOk(cache, status, "literal-pool range")) {
    return false;
  }

  StackBuffer cache_stack;
  gaby_vm::Simulator cache_sim(&cache, cache_stack.data(), cache_stack.size());
  cache_sim.RunFrom(entry);
  const uint64_t cache_x0 = cache_sim.Read(gaby_vm::GpRegister::X0);

  StackBuffer debug_stack;
  gaby_vm::Simulator debug_sim(nullptr, debug_stack.data(), debug_stack.size());
  debug_sim.DebugRunFrom(entry);
  const uint64_t debug_x0 = debug_sim.Read(gaby_vm::GpRegister::X0);

  bool ok = true;
  ok &= ExpectEqU64(cache_x0, kExpectedLiteral, "cache track loaded literal");
  ok &= ExpectEqU64(debug_x0, kExpectedLiteral, "debug track loaded literal");
  ok &= ExpectEqU64(cache_x0, debug_x0, "cache track matches debug track");
  return ok;
}

[[noreturn]] void RunBranchIntoDataInChild() {
  // Layout:
  //   0: nop
  //   1: b +4               ; deliberately enter the embedded data word
  //   2: data low word      ; must abort on cache-track execute
  //   3: ret                ; would terminate only if the abort failed
  alignas(8) const std::array<uint32_t, 4> code = {{
      kNop,
      0x14000001,  // b #+4
      kUnallocatedDataWord,
      kRet,
  }};

  gaby_vm::PredecodeCache cache;
  const gaby_vm::PredecodeCache::RegistrationStatus status =
      cache.RegisterCodeRange(code.data(), code.size() * sizeof(uint32_t));
  if (status != gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    std::fprintf(stderr,
                 "child: RegisterCodeRange returned %s before "
                 "branch-into-data\n",
                 StatusName(status));
    std::_Exit(0);
  }

  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
  sim.RunFrom(reinterpret_cast<uintptr_t>(code.data()));

  std::fprintf(stderr, "child: branch into data returned without aborting\n");
  std::_Exit(0);
}

bool RunBranchIntoDataAbortsOnCacheTrack() {
  const pid_t child = fork();
  if (child < 0) {
    std::perror("fork");
    return false;
  }
  if (child == 0) {
    RunBranchIntoDataInChild();
  }

  int status = 0;
  if (waitpid(child, &status, 0) < 0) {
    std::perror("waitpid");
    return false;
  }

  if (WIFSIGNALED(status)) {
    std::printf(
        "predecode_cache_data_in_stream: child died from signal %d "
        "(expected branch-into-data abort)\n",
        WTERMSIG(status));
    return true;
  }

  if (WIFEXITED(status)) {
    std::fprintf(stderr,
                 "[FAIL] child exited normally with status %d — cache-track "
                 "branch into data did not abort\n",
                 WEXITSTATUS(status));
    return false;
  }

  std::fprintf(stderr,
               "[FAIL] child terminated in an unexpected way (status=%d)\n",
               status);
  return false;
}

}  // namespace

int main() {
  int passed = 0;
  int total = 0;

  total += 1;
  if (RunLiteralPoolLoadOnBothTracks()) {
    passed += 1;
  }

  total += 1;
  if (RunBranchIntoDataAbortsOnCacheTrack()) {
    passed += 1;
  }

  std::printf("predecode_cache_data_in_stream: %d/%d checks passed\n",
              passed,
              total);
  return (passed == total) ? 0 : 1;
}
