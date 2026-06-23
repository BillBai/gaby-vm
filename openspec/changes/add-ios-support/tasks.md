## 1. Spikes — decide the path before committing structure

- [x] 1.1 Spike A: `cmake -G Xcode` generates the `gaby_vm` library project; it builds clean for both `iphonesimulator` arm64 (platform 7, minos 14.0) and `iphoneos` arm64 (platform 2, minos 13.0), static lib needs no signing. Chosen consumption: the library project stays CMake-generated (single-sourced); the XcodeGen host references it as an external project; `generate.sh` runs both generators (design Decision 2, resolved).
- [ ] 1.2 Spike A: confirm the vixl_port crash-guard signal handlers (SIGSEGV/SIGBUS/SIGILL/SIGABRT/SIGALRM) coexist with XCTest's handlers on the Simulator; if they conflict, define the iOS non-signal reporting mode. Verify a deliberately-crashing body is still reported, not lost.
- [x] 1.3 Spike A: runner builds and runs at iOS deployment target 13.0 with C++20; the gaby_vm public headers (which use `std::span`/`std::variant`) compile into the XCTest bundle. Recorded floor: device 13.0; arm64 **simulator** is effectively 14.0 (the inherent Apple-silicon-simulator minimum), which is fine.
- [ ] 1.4 Spike B: confirm `MAP_JIT` / native baseline runs on the iOS Simulator and on a dev-signed, debugger-attached device; record where it works and where it must degrade (design Decision 5).

## 2. Callable entry points — host stays green throughout

- [ ] 2.1 Add a failing host test that drives one correctness suite through a new callable entry (e.g. `run_vixl_port_integer()`) before refactoring.
- [ ] 2.2 Extract each correctness suite body into a callable entry; reduce its CTest executable to a thin `main()` over it. Verify `ctest --test-dir build/debug -R vixl_port` and the full CTest suite stay green.
- [ ] 2.3 Extract each bench kernel into a callable entry; bench executables dispatch to it. Verify `bench_business --verify` is OK and the bench reports are unchanged.

## 3. iOS runner project

- [x] 3.1 Created `ios-runner/`: committed `project.yml` (XcodeGen, host app + XCTest), minimal host app (`App/main.m`), `generate.sh` (runs `cmake -G Xcode` → strips legacy `PBXBuildStyle` via a structural plist edit → `xcodegen`). The host references the CMake project (`projectReferences`) and depends on its `gaby_vm` target; Xcode builds the library per destination and links it. Generated `.xcodeproj`s are git-ignored.
- [x] 3.2 Smoke-validated end to end: `xcodebuild test` on an arm64 iOS Simulator is green — `testLibraryLinks` constructs `PredecodeCache` (forces the link; proves the CMake-built libgaby_vm.a was built as the cross-project dependency and linked), `testVersionIsNonEmpty` passes too.
- [ ] 3.3 Add XCTest cases that call the correctness-suite entries (one suite → pass/fail). Verify `xcodebuild test` on an arm64 Simulator runs all suites green.
- [ ] 3.4 Add the benchmark run path in the runner that calls the bench entries and emits the harness report; attempt native baseline per Spike B with the explicit "not run" fallback. Verify numbers (or the explicit absence) appear.
- [ ] 3.5 Signing: simulator builds need no identity; device signing comes from an untracked xcconfig / automatic signing; no team or identity committed. Verify a clean checkout builds for the Simulator with no signing config.

## 4. CI — iOS Simulator gate

- [ ] 4.1 Write `ci/ios-test.sh`: arch check; explicit loud SKIP (job summary + sticky comment, exit 0) when not arm64; else select an arm64 iOS Simulator destination and run `xcodebuild test`, non-zero on test failure. Reuse `ci/util.sh`. Verify it gates locally on arm64 and prints the explicit skip line when arch is forced non-arm64.
- [ ] 4.2 Add an `ios-sim-test` job to `.github/workflows/ci.yml` (macos-14) that only invokes `ci/ios-test.sh` and feeds the report/sticky comment. Verify the YAML inlines no build/test logic.

## 5. CI — iOS benchmark report

- [ ] 5.1 Write `ci/ios-bench.sh`: same arch/skip logic; run the iOS bench on the arm64 Simulator; emit the report; never gate. Verify it reports locally and the skip path is explicit.
- [ ] 5.2 Add the iOS bench step to `.github/workflows/bench.yml` (main + manual dispatch, report-only). Verify it does not run on pull requests.

## 6. Docs

- [ ] 6.1 Update `docs/refs/ci.md`: the new iOS job, the explicit-skip semantics, and the local reproduce commands.
- [ ] 6.2 Add English `docs/ios.md`: building/testing/benching on iOS, the local device flow, signing, and the native-baseline-on-iOS caveat.

## 7. Verification

- [ ] 7.1 Full host CTest green and `bench_business --verify` OK — no regression from the entry refactor.
- [ ] 7.2 `xcodebuild test` green on an arm64 iOS Simulator; iOS bench produces numbers.
- [ ] 7.3 Local device: suites + bench run from Xcode on a connected iPhone (manual; record the numbers).
- [ ] 7.4 `openspec validate add-ios-support --strict` passes; cross-links in docs resolve.
