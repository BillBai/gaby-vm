// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Entry points for the baseline / unit suites on the iOS runner. Each is the
// suite's main() renamed (via -Dmain=<entry> in the GABY_VM_BUILD_IOS_RUNNER
// libraries; see test/CMakeLists.txt) so the iOS XCTest can call it. Returns 0
// iff the suite passes, exactly like the suite's CTest executable. This header
// is intentionally dependency-free so the XCTest pulls in no gaby_vm or test
// internals. The two fork()-based death tests (typed_register_io_abort,
// simulator_constructor_stack) have no entry here — they cannot run in a single
// sandboxed XCTest process.

#ifndef GABY_VM_TEST_IOS_RUNNER_ENTRIES_H_
#define GABY_VM_TEST_IOS_RUNNER_ENTRIES_H_

int gaby_vm_ios_run_smoke();
int gaby_vm_ios_run_simulator_smoke();
int gaby_vm_ios_run_instructions_constexpr_smoke();
int gaby_vm_ios_run_simulator_correctness();
int gaby_vm_ios_run_predecode_cache_data_in_stream();
int gaby_vm_ios_run_reentrancy();
int gaby_vm_ios_run_shadow_runner();
int gaby_vm_ios_run_workload_shadow();
int gaby_vm_ios_run_typed_register_io();
int gaby_vm_ios_run_branch_hook_dispatch();
int gaby_vm_ios_run_branch_hook_reentrancy();

// Report-only benchmark: runs the business microkernels in cache then decoder
// mode and prints the key:value report to stdout. Always returns 0 (never a
// pass/fail gate). Defined in bench/business.cc; the iOS runner has no
// native-baseline track (that needs JIT), so it is cache-vs-decoder only.
int gaby_vm_ios_run_business_bench();

#endif  // GABY_VM_TEST_IOS_RUNNER_ENTRIES_H_
