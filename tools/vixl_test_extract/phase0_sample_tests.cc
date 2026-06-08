// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// A tiny VIXL-style test file consumed by the capture macros. It proves the
// extraction pipeline end-to-end (capture -> generate -> replay -> double
// oracle) before the tool is pointed at the real upstream test .cc files.
#include "capture_macros.h"

namespace vixl {
namespace aarch64 {

TEST(extract_add) {
  SETUP();
  START();
  __ Mov(x1, 2);
  __ Mov(x2, 3);
  __ Add(x0, x1, x2);
  END();
  RUN();
  ASSERT_EQUAL_64(5, x0);
}

TEST(extract_sub_flags) {
  SETUP();
  START();
  __ Mov(x1, 7);
  __ Subs(x0, x1, 7);  // 7 - 7 = 0 -> Z set, and no borrow -> C set
  END();
  RUN();
  ASSERT_EQUAL_64(0, x0);
  ASSERT_EQUAL_NZCV(ZCFlag);
}

}  // namespace aarch64
}  // namespace vixl
