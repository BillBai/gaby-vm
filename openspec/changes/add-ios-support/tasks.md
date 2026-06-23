## 1. Spikes — decide the path before committing structure

- [x] 1.1 Spike A: `cmake -G Xcode` generates the `gaby_vm` library project; it builds clean for both `iphonesimulator` arm64 (platform 7, minos 14.0) and `iphoneos` arm64 (platform 2, minos 13.0), static lib needs no signing. Chosen consumption: the library project stays CMake-generated (single-sourced); the XcodeGen host references it as an external project; `generate.sh` runs both generators (design Decision 2, resolved).
- [x] 1.2 Spike A resolved: the vixl_port crash-guard signal handlers coexist with XCTest. The XCTest saves the six `sigaction`s before the run and restores them after, containing the guard's handlers to the suite. The integer family ran green on the Simulator (192 passed / 66 skipped / 0 FAILED, matching the macOS Debug baseline exactly) — no signal fired and XCTest reported success.
- [x] 1.3 Spike A: runner builds and runs at iOS deployment target 13.0 with C++20; the gaby_vm public headers (which use `std::span`/`std::variant`) compile into the XCTest bundle. Recorded floor: device 13.0; arm64 **simulator** is effectively 14.0 (the inherent Apple-silicon-simulator minimum), which is fine.
- [x] 1.4 Spike B descoped by decision: the iOS benchmark is **cache vs decoder only** — no native baseline. `MAP_JIT` is not pursued (it would need JIT-entitlement plumbing for marginal value; the on-device cache/decoder absolute numbers are what matter for tater). The "slowdown-vs-native" denominator stays a macOS-arm64 measurement.

## 2. Callable entry points — host stays green throughout

- [x] 2.1 The callable entry (`run_vixl_port_integer()`) is exercised by the existing `vixl_port_integer` CTest, which calls it from its thin `main()` and stays green — so the refactor is covered by existing tests. (A separate failing-test-first step was moot: this is a pure extraction, not new behaviour.)
- [x] 2.2 Each suite reachable as a callable entry. vixl_port: integer/fp/neon expose `run_vixl_port_<family>()` (same TU as their registrations) plus an iOS-only `run_vixl_port_all()` that walks the combined registered set once under a summed baseline (all three link into one XCTest process). Baseline/unit suites: built under `GABY_VM_BUILD_IOS_RUNNER` as libraries with `main()` renamed to `gaby_vm_ios_run_<name>()` (no source edits; their CTest exes are unchanged). The two fork()-based death tests are excluded (iOS sandbox forbids fork). Full host CTest stays green (22/22).
- [x] 2.3 The bench kernels are already shared through `gaby_vm_bench::RunBenchmark` + the `kKernels` table; the iOS entry (`gaby_vm_ios_run_business_bench()`) drives the same table through the same runner, so no per-kernel extraction was needed. `bench_business --verify` stays OK and the host bench reports are unchanged.

## 3. iOS runner project

- [x] 3.1 Created `ios-runner/`: committed `project.yml` (XcodeGen, host app + XCTest), minimal host app (`App/main.m`), `generate.sh` (runs `cmake -G Xcode` → strips legacy `PBXBuildStyle` via a structural plist edit → `xcodegen`). The host references the CMake project (`projectReferences`) and depends on its `gaby_vm` target; Xcode builds the library per destination and links it. Generated `.xcodeproj`s are git-ignored.
- [x] 3.2 Smoke-validated end to end: `xcodebuild test` on an arm64 iOS Simulator is green — `testLibraryLinks` constructs `PredecodeCache` (forces the link; proves the CMake-built libgaby_vm.a was built as the cross-project dependency and linked), `testVersionIsNonEmpty` passes too.
- [x] 3.3 XCTest runs every suite green on an arm64 Simulator: `VixlPortTests` runs the combined integer+fp+neon set (520/69/0, matching the summed macOS baseline) and `BaselineSuiteTests` runs all 11 baseline/unit suites — 15 test cases, 0 failures. The two fork()-based death tests are the only exclusions.
- [x] 3.4 `bench/business.cc` exposes `gaby_vm_ios_run_business_bench()` (built as a library under `GABY_VM_BUILD_IOS_RUNNER`, main renamed); `BenchTests.testBusinessBenchmark` runs every business kernel in cache then decoder mode and prints the harness report. Report-only — no native baseline on iOS. Verified locally: cache ≈ 313–331 ns/insn, decoder ≈ 2.6 µs/insn on the Simulator.
- [x] 3.5 Simulator builds set `CODE_SIGNING_ALLOWED=NO` and need no team — verified by every Simulator run (a clean regenerate builds and tests with no signing config). No team or identity is committed; device signing is documented as coming from the developer's local team (`docs/ios.md`). The on-device build itself is part of 7.3 (needs hardware).

## 4. CI — iOS Simulator gate

- [x] 4.1 `ci/ios-test.sh`: arch check with an explicit loud SKIP (exit 0) when not arm64; else generate the runner (installs xcodegen on demand), pick the first available iPhone Simulator, run `xcodebuild test`, non-zero on failure. Reuses `ci/util.sh`. Verified locally: `Executed 14 tests, with 0 failures`, `### ✅ iOS Simulator tests` in the report.
- [x] 4.2 Added an `iOS Simulator tests` step to `ci.yml`'s `build-test-size` job (macos-14) that only invokes `ci/ios-test.sh`. A step, not a separate job, so its result joins the single sticky PR comment. No build/test logic inlined in the YAML.

## 5. CI — iOS benchmark report

- [x] 5.1 `ci/ios-bench.sh`: same arch/skip logic as the test gate; runs only `BenchTests` on the arm64 Simulator (`-only-testing`), extracts the key:value report into the summary, and always exits 0 (report-only). Verified locally: cache/decoder numbers captured for all five kernels.
- [x] 5.2 Added an `iOS benchmark (Simulator, report-only)` step to `bench.yml` (push to main + manual dispatch only). It does not run on pull requests (bench.yml has no `pull_request` trigger). The PR test gate skips `BenchTests` via `-skip-testing`, so timing never gates.

## 6. Docs

- [x] 6.1 Updated `docs/refs/ci.md`: the iOS test gate + iOS bench in the workflow table, `ci/ios-test.sh`/`ci/ios-bench.sh` in the scripts table, the explicit-skip note, and the local reproduce commands. Also refreshed the README's stale "iOS demo to come" line.
- [x] 6.2 Added English `docs/ios.md`: one-command setup, running tests/bench on the Simulator and on a device, signing, the build option, the excluded fork-based death tests, and the no-native-baseline-on-iOS caveat.

## 7. Verification

- [x] 7.1 Full host CTest green (22/22) and `bench_business --verify` OK after the entry/CMake refactors — no host regression.
- [x] 7.2 `ci/ios-test.sh` green on the arm64 Simulator: 14 tests, 0 failures (correctness suites; `BenchTests` skipped by the gate). `ci/ios-bench.sh` produces cache/decoder numbers for all five kernels.
- [ ] 7.3 Local device: suites + bench run from Xcode on a connected iPhone (manual; record the numbers). Left for a developer with a device — the runner builds for `iphoneos` arm64; device signing comes from the local team (see `docs/ios.md`).
- [x] 7.4 `openspec validate add-ios-support --strict` passes; docs cross-links resolve.
