// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// CTest entry for ported integer-family VIXL tests. In Phase 0 it runs the
// hand-authored fixtures; from Phase 1 it includes the generated integer set.
#include <cstddef>
#include <cstdio>

#include "vixl_port_runner.h"

// Integer/logical/loadstore/branch family, machine-extracted from the upstream
// test-assembler-aarch64.cc. Regenerate via tools/vixl_test_extract; see the
// sibling manifest_integer.md for the included/skipped breakdown.
#include "generated/integer_fixtures.inc"

int main() {
  using namespace gaby_vm::vixl_port;
  size_t count = 0;
  const PortedFixture* fixtures = IntegerFixtures(&count);

  RunStats stats;
  RunAll(fixtures, count, stats);

  std::printf("vixl_port_integer: %d/%d ported cases passed\n",
              stats.passed,
              stats.cases);
  return (stats.passed == stats.cases) ? 0 : 1;
}
