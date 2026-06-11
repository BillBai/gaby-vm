// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// G4 verification (tasks.md 4.4): a trivial, hand-written TEST() body driven
// through the two-track macros + oracle, proving the harness compiles, links
// against the island, and passes on a known-good body — before the real
// upstream families are wired in (§5). Uses the exact macro surface the
// upstream test .cc expects, in the same namespace, so it exercises
// SETUP/START/END/RUN and the scalar ASSERT_EQUAL_* path end to end.
#include "gaby_two_track_macros.h"

namespace vixl {
namespace aarch64 {

TEST(harness_smoke_scalar) {
  SETUP();

  START();
  __ Mov(x0, 0x1234);
  __ Add(x1, x0, 0x10);
  __ Mov(w2, 0xabcd);
  __ Add(x3, x0, x0);
  __ Sub(x4, x1, x0);
  END();

  if (CAN_RUN()) {
    RUN();
    ASSERT_EQUAL_64(0x1234, x0);
    ASSERT_EQUAL_64(0x1244, x1);
    ASSERT_EQUAL_32(0xabcd, w2);
    ASSERT_EQUAL_64(0x2468, x3);
    ASSERT_EQUAL_64(0x10, x4);
  }
}

// Self-check for the frame-window memory oracle (review issue I4): the scalar
// body above never stores, so it does not exercise the window mechanism at all.
// This body bakes the host address of a body-local buffer into the stream and
// stores through it, so all three engines run a read-modify-write against the
// same locals. That only produces a consistent, correct result if the harness
// (a) snapshots+resets the window between engine runs — otherwise an earlier
// engine's store leaks into a later engine's load and the differential oracle
// diverges — and (b) captures+compares the window afterwards. If either half of
// the mechanism regressed, this body would FAIL rather than pass, so it is the
// smoke gate for the memory oracle itself.
TEST(harness_smoke_memory) {
  SETUP();

  uint64_t data[2] = {0xdead0000dead0000, 0};

  START();
  __ Mov(x0, reinterpret_cast<uint64_t>(&data[0]));
  __ Ldr(x1, MemOperand(x0));  // load the initial value (reset each run)
  __ Add(x1, x1, 0x11);        // modify
  __ Str(x1, MemOperand(x0));  // store back to data[0]
  __ Mov(x2, 0xcafef00d);
  __ Str(x2, MemOperand(x0, 8));  // store a fresh value to data[1]
  END();

  if (CAN_RUN()) {
    RUN();
    ASSERT_EQUAL_64(0xdead0000dead0011, x1);
    ASSERT_EQUAL_64(0xcafef00d, x2);
    // data[0]/data[1] results are checked by the frame-window memory oracle
    // (all three engines compared), which is the point of this body.
  }
}

}  // namespace aarch64
}  // namespace vixl

#include "gaby_two_track_main.h"

int main() {
  return gaby_vm::vixl_port_live::RunRegisteredTests("harness_smoke");
}
