// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// simulator_constructor_stack_test: minimum stack-size contract on the
// gaby_vm::Simulator constructor.
//
// The constructor MUST abort when stack_size is below
// gaby_vm::Simulator::kMinStackSize. The abort path goes through
// VIXL_ABORT_WITH_MSG → std::abort → SIGABRT, so each rejection case runs in
// a forked child and the parent reports pass iff the child died from a
// signal. The success case (stack_size == kMinStackSize) runs in-process and
// checks the constructor returns a Simulator whose initial SP equals the
// 16-byte-aligned top of the buffer.
//
// Same forking pattern as typed_register_io_abort_test.cc; that test's
// header comment explains why CTest's WILL_FAIL property cannot replace it.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

namespace {

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

[[noreturn]] void construct_with_bad_size_in_child(size_t bad_size) {
  // Buffer must exist (the constructor reads stack_buffer to compute the
  // would-be SP — though it should abort before that). Size of the actual
  // backing memory is irrelevant to the contract: stack_size, not the host
  // allocation, is what the check sees.
  alignas(16) std::array<uint8_t, 16 * 1024> buffer{};

  gaby_vm::Simulator sim(nullptr, buffer.data(), bad_size);

  // If construction returned, the contract is broken. Exit cleanly so the
  // parent reports the case as a failure (signal-or-bust).
  std::fprintf(stderr,
               "child: Simulator(stack_size=%zu) returned without aborting\n",
               bad_size);
  std::_Exit(0);
}

// Runs one rejection case in a forked child; returns true when the child
// died from a signal (the expected outcome).
bool run_rejection_case(size_t bad_size, const char* label) {
  const pid_t child = fork();
  if (child < 0) {
    std::perror("fork");
    return false;
  }
  if (child == 0) {
    construct_with_bad_size_in_child(bad_size);
  }

  int status = 0;
  if (waitpid(child, &status, 0) < 0) {
    std::perror("waitpid");
    return false;
  }

  if (WIFSIGNALED(status)) {
    std::printf(
        "rejection (%s, stack_size=%zu): child died from signal %d "
        "(expected — minimum-stack-size check tripped)\n",
        label,
        bad_size,
        WTERMSIG(status));
    return true;
  }
  if (WIFEXITED(status)) {
    std::fprintf(stderr,
                 "[FAIL] rejection (%s, stack_size=%zu): child exited normally "
                 "with status %d — constructor did not abort on the "
                 "deliberately undersized stack buffer\n",
                 label,
                 bad_size,
                 WEXITSTATUS(status));
    return false;
  }
  std::fprintf(stderr,
               "[FAIL] rejection (%s, stack_size=%zu): child terminated in an "
               "unexpected way (status=%d)\n",
               label,
               bad_size,
               status);
  return false;
}

// Construct at exactly kMinStackSize and verify the SP lands on the expected
// 16-byte-aligned top of the buffer. Runs in-process; a regression would
// either fail the SP check here or crash the test executable.
void run_success_case() {
  alignas(16) std::array<uint8_t, gaby_vm::Simulator::kMinStackSize> buffer{};

  gaby_vm::Simulator sim(nullptr, buffer.data(), buffer.size());
  const uint64_t sp = sim.Read(gaby_vm::GpRegister::SP);

  const uintptr_t top =
      reinterpret_cast<uintptr_t>(buffer.data()) + buffer.size();
  const uint64_t expected_sp = static_cast<uint64_t>(top & ~uintptr_t{15});

  check(sp == expected_sp,
        "success (stack_size == kMinStackSize): SP equals 16-aligned top");
}

}  // namespace

int main() {
  check(run_rejection_case(0, "zero"),
        "rejection (zero): constructor aborts on stack_size = 0");
  check(run_rejection_case(16, "tiny"),
        "rejection (tiny): constructor aborts on stack_size = 16");
  check(run_rejection_case(gaby_vm::Simulator::kMinStackSize - 1,
                           "just-below-floor"),
        "rejection (just-below-floor): constructor aborts on "
        "stack_size = kMinStackSize - 1");
  run_success_case();

  std::printf("simulator_constructor_stack: %d/%d checks passed\n",
              g_passed,
              g_total);
  return (g_passed == g_total) ? 0 : 1;
}
