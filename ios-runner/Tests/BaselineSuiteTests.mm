// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Runs the baseline / unit suites on iOS. Each suite is the same source as its
// host CTest executable, built as a library with main() renamed to the entry
// called below (see test/CMakeLists.txt and test/ios_runner_entries.h). These
// suites exercise platform-independent behaviour (public API, hand-encoded
// sequences, internal mechanisms); running them here confirms they hold on the
// iOS toolchain and runtime too. The two fork()-based death tests are excluded
// (the iOS sandbox forbids fork and one XCTest process cannot host them).

#import <XCTest/XCTest.h>

#include "ios_runner_entries.h"

@interface BaselineSuiteTests : XCTestCase
@end

@implementation BaselineSuiteTests

- (void)testSmoke {
  XCTAssertEqual(gaby_vm_ios_run_smoke(), 0);
}

- (void)testSimulatorSmoke {
  XCTAssertEqual(gaby_vm_ios_run_simulator_smoke(), 0);
}

- (void)testInstructionsConstexprSmoke {
  XCTAssertEqual(gaby_vm_ios_run_instructions_constexpr_smoke(), 0);
}

- (void)testSimulatorCorrectness {
  XCTAssertEqual(gaby_vm_ios_run_simulator_correctness(), 0);
}

- (void)testReentrancy {
  XCTAssertEqual(gaby_vm_ios_run_reentrancy(), 0);
}

- (void)testShadowRunner {
  XCTAssertEqual(gaby_vm_ios_run_shadow_runner(), 0);
}

- (void)testWorkloadShadow {
  XCTAssertEqual(gaby_vm_ios_run_workload_shadow(), 0);
}

- (void)testTypedRegisterIo {
  XCTAssertEqual(gaby_vm_ios_run_typed_register_io(), 0);
}

- (void)testBranchHookDispatch {
  XCTAssertEqual(gaby_vm_ios_run_branch_hook_dispatch(), 0);
}

- (void)testBranchHookReentrancy {
  XCTAssertEqual(gaby_vm_ios_run_branch_hook_reentrancy(), 0);
}

@end
