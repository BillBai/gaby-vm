// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Report-only benchmark on iOS. Runs the business-logic microkernels in cache
// then decoder mode and prints the harness's key:value report to stdout, which
// ci/ios-bench.sh captures. This is NOT a gate: ci/ios-test.sh skips this class
// (-skip-testing), and ci/ios-bench.sh runs only this class (-only-testing) on
// main / manual dispatch, never failing on a timing value. The iOS runner has
// no native-baseline track (that needs JIT), so the numbers are cache vs
// decoder — the real on-device interpreter speed, just without the
// slowdown-vs-native denominator.

#import <XCTest/XCTest.h>

#include "ios_runner_entries.h"

@interface BenchTests : XCTestCase
@end

@implementation BenchTests

- (void)testBusinessBenchmark {
  const int rc = gaby_vm_ios_run_business_bench();
  XCTAssertEqual(rc, 0, @"business benchmark should run to completion");
}

@end
