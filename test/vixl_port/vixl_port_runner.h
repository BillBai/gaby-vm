// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Generic replay harness for ported VIXL simulator tests. Runs each fixture
// on both gaby_vm tracks (cache via RunFrom, decoder via DebugRunFrom) and
// applies two oracles:
//   - differential: full RegisterFile must match between the two tracks;
//   - absolute: every harvested ASSERT_EQUAL_* target must hold on each track.
// Uses only the gaby_vm public API.
#ifndef GABY_VM_TEST_VIXL_PORT_RUNNER_H_
#define GABY_VM_TEST_VIXL_PORT_RUNNER_H_

#include <cstddef>

#include "vixl_port_fixture.h"

namespace gaby_vm::vixl_port {

struct RunStats {
  int cases = 0;
  int passed = 0;  // a case passes only if BOTH oracles hold on BOTH tracks
};

// Runs an array of fixtures, accumulating into stats. Each fixture is replayed
// on both tracks under the differential + absolute oracles; prints [FAIL]
// detail to stderr on any violation. A case counts as passed only if both
// oracles hold on both tracks.
void RunAll(const PortedFixture* fixtures, size_t count, RunStats& stats);

}  // namespace gaby_vm::vixl_port

#endif  // GABY_VM_TEST_VIXL_PORT_RUNNER_H_
