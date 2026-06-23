// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Runs the real vixl_port correctness suite on iOS. The host CTest path runs
// each family (integer / fp / neon) as its own process; the iOS runner links
// all three into one XCTest bundle, so it runs the *combined* registered set
// once via run_vixl_port_all() under the summed "ios_runner_all" coverage
// baseline. The suite logic is shared with the host path; this file pulls in no
// VIXL or harness header beyond the dependency-free vixl_port_entries.h.

#import <XCTest/XCTest.h>

#include <csignal>

#include "vixl_port_entries.h"

@interface VixlPortTests : XCTestCase
@end

@implementation VixlPortTests

- (void)testVixlPortAllFamilies {
  // The vixl_port crash guard installs its own handlers (sigaltstack +
  // sigsetjmp) for these signals while it runs each body. Save them before the
  // run and restore them after, so the guard's handlers are contained to the
  // suite and XCTest's own crash reporting is left intact. On a green run no
  // handler ever fires — this is hygiene, not a workaround.
  const int kGuardedSignals[] = {SIGABRT, SIGSEGV, SIGFPE,
                                 SIGBUS,  SIGILL,  SIGALRM};
  constexpr int kCount = sizeof(kGuardedSignals) / sizeof(kGuardedSignals[0]);
  struct sigaction saved[kCount];
  for (int i = 0; i < kCount; ++i) {
    sigaction(kGuardedSignals[i], nullptr, &saved[i]);
  }

  const int rc = gaby_vm::ios_runner::run_vixl_port_all();

  for (int i = 0; i < kCount; ++i) {
    sigaction(kGuardedSignals[i], &saved[i], nullptr);
  }

  XCTAssertEqual(rc, 0,
                 @"vixl_port suite (integer+fp+neon, both tracks, both oracles, "
                 @"summed coverage baseline) must be green on iOS");
}

@end
