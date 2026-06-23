## Why

gaby-vm targets iOS first, yet nothing in the project builds, tests, or
benchmarks on iOS today: CI runs only on macOS arm64, and there is no way to
validate the interpreter on a real iPhone. The library now cross-compiles
clean for iOS arm64 (verified: `platform=iOS`, `minos 13.0`, static lib). The
next step toward embedding into the tater hot-fix app is to make iOS a
first-class, continuously-tested platform.

## What Changes

- Add `ios-runner/`: a committed, thin Xcode project (Host App target + XCTest
  target) that runs the full correctness suites and the benchmark harness on
  iOS — the iOS Simulator in CI, a physical device locally.
- Refactor each correctness suite and benchmark kernel to expose a callable
  entry function. The existing host CTest/bench executables become thin
  `main()`s over those entries; the iOS XCTest cases and bench runner call the
  same entries. One logic, two drivers (host = CTest, iOS = XCTest).
- The runner consumes the library through a CMake-generated Xcode subproject
  (source list stays single-sourced in CMake). Fallback if that proves too
  awkward for the sim/device split: compile gaby_vm sources directly in the
  runner, guarded by a source-list drift check against CMake. No prebuilt-`.a`
  integration.
- CI: new portable `ci/ios-test.sh` and `ci/ios-bench.sh`. `ci.yml` gains an
  `ios-sim-test` job (deterministic gate — blocks merge on test failure).
  `bench.yml` gains an iOS bench step (report-only). Both **explicitly SKIP** —
  loudly, in the job summary and the sticky comment — when the runner is not
  arm64 (e.g. an x86 runner with no arm64 simulator). A skip is never a silent
  pass.
- Benchmark native baseline runs on iOS where executable memory is available
  (the runner is always development-signed and debugger-attached, so iOS
  permits `MAP_JIT`; the simulator is a host process). Where it is denied, the
  run degrades to cache/decoder only and reports that native was not run.
- Docs: update [`docs/refs/ci.md`](../../../docs/refs/ci.md); add English
  `docs/ios.md` (iOS build/test/bench + local device flow + signing notes).

## Capabilities

### New Capabilities

- `ios-runner`: the committed iOS Host App + XCTest project that executes
  gaby-vm's correctness suites and benchmark harness on the iOS Simulator and
  on a physical device, including how it consumes the library and how the
  suites and benches are driven.

### Modified Capabilities

- `continuous-integration`: adds the iOS Simulator test gate, the iOS
  benchmark report, and the explicit-skip-on-non-arm64 semantics.
- `benchmark-harness`: the harness becomes drivable from the iOS runner; on
  iOS the native baseline is optional and its absence is reported explicitly.

## Impact

- New: `ios-runner/` (Xcode project + app/test sources), `ci/ios-test.sh`,
  `ci/ios-bench.sh`, `docs/ios.md`.
- Modified: `test/` and `bench/` (extract callable entry points; existing
  `main()`s become thin wrappers), `.github/workflows/ci.yml` and `bench.yml`,
  `docs/refs/ci.md`.
- No change to the shipped library API or the imported VIXL boundary. Source
  integration into tater (git submodule) is unaffected — the runner and tests
  stay top-level / dev-only and are already OFF when gaby-vm is a subdirectory.

## Out of Scope

- Automated real-device runs in CI (device test/bench is local-only for now).
- Linux / Android / HarmonyOS CI (separate follow-up).
- Distribution-signed (App Store) execution of the runner; it is always a
  development build.
- Any change to interpreter semantics or instruction coverage.
