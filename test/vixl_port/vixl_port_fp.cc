// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// CTest entry for ported floating-point-family VIXL tests. Replays the machine-
// extracted fp fixtures (from test-assembler-fp-aarch64.cc) on both gaby tracks
// under the differential + absolute double oracle. Regenerate via
// tools/vixl_test_extract; see the sibling manifest_fp.md.
#include <cstddef>
#include <cstdio>

#include "generated/fp_fixtures.inc"
#include "vixl_port_runner.h"

int main() {
  using namespace gaby_vm::vixl_port;
  size_t count = 0;
  const PortedFixture* fixtures = FpFixtures(&count);

  RunStats stats;
  RunAll(fixtures, count, stats);

  std::printf("vixl_port_fp: %d/%d ported cases passed\n",
              stats.passed,
              stats.cases);
  return (stats.passed == stats.cases) ? 0 : 1;
}
