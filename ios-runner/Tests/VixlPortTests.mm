// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Runs a real vixl_port correctness family on iOS. The suite logic is shared
// with the host CTest path: both call the same run_vixl_port_<family>() entry
// (defined in the family TU, alongside its registered TEST() bodies). The
// XCTest here is just the iOS driver — it pulls in no VIXL or harness header
// beyond the dependency-free vixl_port_entries.h.

#import <XCTest/XCTest.h>

#include <csignal>

#include "vixl_port_entries.h"

@interface VixlPortTests : XCTestCase
@end

@implementation VixlPortTests

- (void)testVixlPortIntegerFamily {
  // The vixl_port crash guard installs its own handlers (sigaltstack +
  // sigsetjmp) for these signals while it runs each body. Save them before the
  // run and restore them after, so the guard's handlers are contained to the
  // suite and XCTest's own crash reporting is left intact for the rest of the
  // bundle. (On a green run no handler ever fires; this is hygiene, not a
  // workaround.)
  const int kGuardedSignals[] = {SIGABRT, SIGSEGV, SIGFPE,
                                 SIGBUS,  SIGILL,  SIGALRM};
  constexpr int kCount = sizeof(kGuardedSignals) / sizeof(kGuardedSignals[0]);
  struct sigaction saved[kCount];
  for (int i = 0; i < kCount; ++i) {
    sigaction(kGuardedSignals[i], nullptr, &saved[i]);
  }

  const int rc = gaby_vm::ios_runner::run_vixl_port_integer();

  for (int i = 0; i < kCount; ++i) {
    sigaction(kGuardedSignals[i], &saved[i], nullptr);
  }

  XCTAssertEqual(rc, 0,
                 @"vixl_port integer family must be green on iOS (both tracks, "
                 @"both oracles, coverage baseline met)");
}

@end
