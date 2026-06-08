// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// CTest entry for ported integer-family VIXL tests. In Phase 0 it runs the
// hand-authored fixtures; from Phase 1 it includes the generated integer set.
#include <cstddef>
#include <cstdio>

#include "vixl_port_runner.h"

// Phase 0: machine-extracted fixtures (from the VIXL-style sample tests),
// proving the capture->generate->replay pipeline end-to-end. Replaced by the
// real integer family include in Phase 1.
#include "generated/phase0_extracted.inc"

int main() {
  using namespace gaby_vm::vixl_port;
  size_t count = 0;
  const PortedFixture* fixtures = Phase0Fixtures(&count);

  RunStats stats;
  RunAll(fixtures, count, stats);

  std::printf("vixl_port_integer: %d/%d ported cases passed\n",
              stats.passed,
              stats.cases);
  return (stats.passed == stats.cases) ? 0 : 1;
}
