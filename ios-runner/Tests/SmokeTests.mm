// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Smoke test for the iOS runner pipeline: proves the gaby_vm library (built by
// the CMake-generated project) links into the XCTest bundle and that its public
// API is reachable from Objective-C++ on iOS. Intentionally minimal — it fails
// fast if the CMake-lib + XcodeGen-host wiring is broken, not to exercise
// behaviour. Real correctness suites are wired in once their callable entry
// points land (see tasks 2.x, 3.3).

#import <XCTest/XCTest.h>

#include <cstring>

#include "gaby_vm/gaby_vm.h"
#include "gaby_vm/predecode_cache.h"

@interface SmokeTests : XCTestCase
@end

@implementation SmokeTests

- (void)testVersionIsNonEmpty {
  const char *v = gaby_vm::version();
  XCTAssertTrue(v != nullptr && std::strlen(v) > 0,
                @"gaby_vm::version() should return a non-empty string");
}

- (void)testLibraryLinks {
  // PredecodeCache's constructor/destructor live in the static library
  // (predecode_cache.cc), so constructing one forces the link — unlike the
  // header-only version() above, this fails to build/link if the library is
  // not actually wired in.
  gaby_vm::PredecodeCache cache;
  (void)cache;
}

@end
