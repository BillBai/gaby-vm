// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// typed_register_io_abort_test: span-write fail-fast on out-of-range enum.
//
// Demonstrates the abort path of Simulator::Write(std::span<RegisterWrite>):
// when a span element carries an enum constructed via `static_cast` outside
// the declared range, the call MUST abort with a diagnostic identifying the
// offending element.
//
// VIXL_ABORT_WITH_MSG ultimately calls std::abort(), which raises SIGABRT.
// CTest's WILL_FAIL property does NOT invert "Subprocess aborted" outcomes —
// it inverts ordinary exit-code interpretation only. So the test program
// validates the expectation itself: fork a child, run the bad Write in the
// child, waitpid in the parent, and report pass iff the child died from a
// signal (typically SIGABRT). The test exits 0/1 the normal way, so the
// CTest registration needs no special properties.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <sys/wait.h>
#include <unistd.h>

#include "embedding_stack.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

namespace {

[[noreturn]] void run_bad_write_in_child() {
  StackBuffer stack;
  gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());

  // Construct a deliberately invalid GpRegister via static_cast. 99 is well
  // outside the declared [0,32] range. The variant happily carries it (POD
  // payload), but Simulator::Write(span) MUST detect the out-of-range
  // underlying value, abort, and identify the offending element.
  const std::array<gaby_vm::RegisterWrite, 2> writes =
      {gaby_vm::RegisterWrite{gaby_vm::GpWrite{gaby_vm::GpRegister::X0, 0x1}},
       gaby_vm::RegisterWrite{
           gaby_vm::GpWrite{static_cast<gaby_vm::GpRegister>(99), 0x2}}};

  sim.Write(std::span<const gaby_vm::RegisterWrite>(writes));

  // If we reach this point the implementation failed to abort on the bad
  // element. Exit cleanly so the parent reports a test failure.
  std::fprintf(stderr, "child: Write(span) returned without aborting\n");
  std::_Exit(0);
}

}  // namespace

int main() {
  const pid_t child = fork();
  if (child < 0) {
    std::perror("fork");
    return 1;
  }
  if (child == 0) {
    run_bad_write_in_child();
  }

  int status = 0;
  if (waitpid(child, &status, 0) < 0) {
    std::perror("waitpid");
    return 1;
  }

  if (WIFSIGNALED(status)) {
    const int signum = WTERMSIG(status);
    std::printf(
        "typed_register_io_abort: child died from signal %d (expected — the "
        "span-write fail-fast path took effect)\n",
        signum);
    return 0;
  }

  if (WIFEXITED(status)) {
    std::fprintf(stderr,
                 "[FAIL] child exited normally with status %d — "
                 "Simulator::Write(span) "
                 "did not abort on the deliberately out-of-range "
                 "GpRegister(99)\n",
                 WEXITSTATUS(status));
    return 1;
  }

  std::fprintf(stderr,
               "[FAIL] child terminated in an unexpected way (status=%d)\n",
               status);
  return 1;
}
