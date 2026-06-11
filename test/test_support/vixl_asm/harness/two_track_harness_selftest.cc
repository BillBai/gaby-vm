// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// =============================================================================
// two_track_harness_selftest: regression protection for the two-track harness's
// OWN new safety logic, which has no naturally-occurring trigger in the suite.
//
// Two pieces of harness logic exist for the day an aggressive cache/dispatch
// change misbehaves, but until that day they never run on real input:
//
//   * the gaby-track crash guard — a fatal signal while a GABY track (cache or
//     decoder) executes must FAIL the family and halt it, whereas a signal
//     during assembly or the REFERENCE run is a legitimate skip; and
//   * the coverage baseline gate — a drift in the ran/skipped split must FAIL,
//     unless an explicit rebaseline is in effect.
//
// "Both configs green" only proves the pass-through halves ("signal didn't
// fire" and "counts matched"). This test injects the failure halves and asserts
// the harness reacts correctly — the same philosophy as shadow_runner_test,
// which injects a real cache defect and asserts the oracle reports it.
//
// The crash guard is exercised by injecting a fatal signal at a chosen phase
// via the GABY_VM_VIXL_PORT_SELFTEST seam (a no-op in the real family
// binaries). Each gaby-track scenario runs in its OWN process (one per CTest
// invocation, selected by argv): a gaby-track crash latches the shared engine's
// re-entrancy state, so the harness halts and the engine cannot be soundly
// reused within the process — exactly the property under test.
// =============================================================================

#define GABY_VM_VIXL_PORT_SELFTEST 1

#include "gaby_two_track_macros.h"

// Two trivial, known-good bodies. Whichever the walk reaches first is where the
// once-only injected fault lands; the assertions below depend only on the
// resulting counts, not on which body it is.
namespace vixl {
namespace aarch64 {

TEST(selftest_body_a) {
  SETUP();
  START();
  __ Mov(x0, 0x11);
  END();
  if (CAN_RUN()) {
    RUN();
    ASSERT_EQUAL_64(0x11, x0);
  }
}

TEST(selftest_body_b) {
  SETUP();
  START();
  __ Mov(x0, 0x22);
  END();
  if (CAN_RUN()) {
    RUN();
    ASSERT_EQUAL_64(0x22, x0);
  }
}

}  // namespace aarch64
}  // namespace vixl

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gaby_two_track_main.h"

namespace {

using gaby_vm::vixl_port_live::RunPhase;
using gaby_vm::vixl_port_live::RunSummary;

int g_passed = 0;
int g_total = 0;

void check(bool ok, const char* label) {
  ++g_total;
  if (ok) {
    ++g_passed;
  } else {
    std::fprintf(stderr, "[FAIL] %s\n", label);
  }
}

// Fire a fatal signal exactly once, the first time the harness enters the
// target phase. "Once" so that in the reference scenario the SECOND body still
// runs and passes (proving a reference-phase fault skips one case without
// halting).
RunPhase g_target_phase = RunPhase::kOutsideRun;
bool g_fault_fired = false;

void InjectFaultAtTarget(RunPhase p) {
  if (!g_fault_fired && p == g_target_phase) {
    g_fault_fired = true;
    std::raise(SIGSEGV);  // stands in for a gaby track's wild PC / fatal fault
  }
}

// Run the family walk with a fault injected at `target`. The "selftest" family
// has no coverage baseline, so set the rebaseline env to keep that gate quiet —
// these scenarios test the crash guard, not the baseline.
RunSummary RunWithFaultAt(RunPhase target) {
  g_target_phase = target;
  g_fault_fired = false;
  gaby_vm::vixl_port_live::g_selftest_phase_fault = InjectFaultAtTarget;
  setenv("VIXL_PORT_REBASELINE", "1", 1);
  return gaby_vm::vixl_port_live::RunRegisteredTestsSummary("selftest");
}

// Scenario: a fatal signal while a gaby track runs FAILs the family and halts
// it.
int scenario_gaby_track(RunPhase target, const char* label) {
  RunSummary s = RunWithFaultAt(target);
  check(s.failed == 1, "gaby-track crash is counted as a FAILURE (not a skip)");
  check(s.halted_on_gaby_crash, "gaby-track crash halts the family");
  check(s.total == 1, "halt stops the walk after the crashing case");
  check(s.ran == 0 && s.skipped == 0,
        "the crashing case is neither ran nor skipped");
  std::fprintf(stderr,
               "  [%s] failed=%d halted=%d total=%d\n",
               label,
               s.failed,
               static_cast<int>(s.halted_on_gaby_crash),
               s.total);
  return (g_passed == g_total) ? 0 : 1;
}

// Scenario: a fatal signal during the REFERENCE run is a skip, not a failure,
// and does not halt — the second body still runs and passes.
int scenario_reference() {
  RunSummary s = RunWithFaultAt(RunPhase::kReference);
  check(s.failed == 0, "reference-run crash is NOT a failure");
  check(!s.halted_on_gaby_crash,
        "reference-run crash does not halt the family");
  check(s.total == 2, "the walk continues past a reference-run crash");
  check(s.skipped == 1, "the crashing case is skipped");
  check(s.ran == 1, "the surviving case still runs and passes");
  std::fprintf(stderr,
               "  [reference] failed=%d halted=%d skipped=%d ran=%d\n",
               s.failed,
               static_cast<int>(s.halted_on_gaby_crash),
               s.skipped,
               s.ran);
  return (g_passed == g_total) ? 0 : 1;
}

// Scenario: the pure coverage-baseline decision. Proves BOTH directions —
// matching counts pass, drift fails, and rebaseline suppresses the failure.
int scenario_baseline() {
  using gaby_vm::vixl_port_live::IsBaselineViolation;
  // expected ran=188, skipped=70 (an arbitrary baseline for the check).
  check(!IsBaselineViolation(188, 70, 188, 70, false),
        "baseline: matching counts are not a violation");
  check(IsBaselineViolation(188, 70, 187, 70, false),
        "baseline: a ran-count drift is a violation");
  check(IsBaselineViolation(188, 70, 188, 71, false),
        "baseline: a skipped-count drift is a violation");
  check(!IsBaselineViolation(188, 70, 187, 70, true),
        "baseline: rebaseline suppresses a drift violation");
  check(!IsBaselineViolation(188, 70, 188, 70, true),
        "baseline: matching counts under rebaseline are not a violation");
  return (g_passed == g_total) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const char* scenario = (argc > 1) ? argv[1] : "";
  int rc = 0;
  if (std::strcmp(scenario, "cache") == 0) {
    rc = scenario_gaby_track(RunPhase::kCacheTrack, "cache");
  } else if (std::strcmp(scenario, "decoder") == 0) {
    rc = scenario_gaby_track(RunPhase::kDecoderTrack, "decoder");
  } else if (std::strcmp(scenario, "reference") == 0) {
    rc = scenario_reference();
  } else if (std::strcmp(scenario, "baseline") == 0) {
    rc = scenario_baseline();
  } else {
    std::fprintf(stderr,
                 "usage: %s <cache|decoder|reference|baseline>\n",
                 argv[0]);
    return 2;
  }
  std::printf("two_track_harness_selftest[%s]: %d/%d checks passed\n",
              scenario,
              g_passed,
              g_total);
  return rc;
}
