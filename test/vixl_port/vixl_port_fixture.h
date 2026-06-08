// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// POD fixture types shared by generated *.inc data and the replay runner.
// No VIXL types here: this header is part of the self-contained, shipping
// test surface and must compile with only the gaby_vm public API in scope.
#ifndef GABY_VM_TEST_VIXL_PORT_FIXTURE_H_
#define GABY_VM_TEST_VIXL_PORT_FIXTURE_H_

#include <cstddef>
#include <cstdint>

#include "gaby_vm/registers.h"

namespace gaby_vm::vixl_port {

// One ASSERT_EQUAL_* target harvested from the upstream VIXL test.
enum class AssertKind : uint8_t {
  kX,     // 64-bit GP register (ASSERT_EQUAL_64 on an X register)
  kW,     // 32-bit GP register (ASSERT_EQUAL_32 on a W register)
  kNZCV,  // condition flags (ASSERT_EQUAL_NZCV)
  kFP32,  // single-precision (ASSERT_EQUAL_FP32 on an S register), bits in
          // expected_lo
  kFP64,  // double-precision (ASSERT_EQUAL_FP64 on a D register), bits in
          // expected_lo
  kV128,  // 128-bit vector (ASSERT_EQUAL_128), expected_hi:expected_lo
};

struct AssertTarget {
  AssertKind kind;
  uint8_t reg;  // register code 0..31 (ignored for kNZCV)
  uint64_t expected_lo;
  uint64_t expected_hi;  // only meaningful for kV128
};

// One ported test case. `code` points at the body words ONLY; the replay
// runner appends a terminating RET (0xd65f03c0) so RunFrom stops on the
// null-LR contract. `entry` is the architectural state at body entry; sp and
// LR are overridden at replay time (sp -> the replay stack, LR -> null).
struct PortedFixture {
  const char* name;
  const uint32_t* code;
  size_t code_words;
  RegisterFile entry;
  const AssertTarget* asserts;
  size_t assert_count;
};

}  // namespace gaby_vm::vixl_port

#endif  // GABY_VM_TEST_VIXL_PORT_FIXTURE_H_
