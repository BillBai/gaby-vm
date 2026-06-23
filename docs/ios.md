# iOS runner

gaby-vm's correctness suites and its business benchmark run on iOS — the iOS
Simulator in CI, a physical device locally — through a small committed Xcode
host under [`ios-runner/`](../ios-runner). This doc is the operational map: how
to generate it, run it, and how it is wired into CI.

The library itself is unchanged: it is the same `gaby_vm` static library, built
for iOS by CMake. Nothing here adds a JIT, executable memory, or any iOS-only
code path — it is the ordinary interpreter, exercised on iOS.

## One command to set it up

The Xcode projects are generated, not committed. Generate them with:

```sh
ios-runner/generate.sh
```

It runs three steps and leaves you an openable project:

1. `cmake -G Xcode` generates the `gaby_vm` library project under
   `build/ios-xcode/` (the source list stays single-sourced in CMake).
2. It strips the legacy `PBXBuildStyle` objects CMake emits — XcodeGen's project
   parser rejects them, Xcode does not need them (a structural plist edit).
3. `xcodegen` generates the host app + XCTest project from
   [`ios-runner/project.yml`](../ios-runner/project.yml), referencing the
   library project and depending on its `gaby_vm` target.

Then `open ios-runner/GabyRunner.xcodeproj`. XcodeGen is needed only to
regenerate (install with `brew install xcodegen`); CI and a plain build use the
generated project directly. Both generated `.xcodeproj` bundles are git-ignored.

## Running the tests

On the **Simulator** (arm64), from the command line:

```sh
xcodebuild test -project ios-runner/GabyRunner.xcodeproj -scheme GabyRunner \
  -destination 'platform=iOS Simulator,name=iPhone 16'
```

Or open the project in Xcode, pick a simulator, and press ⌘U.

What runs (all green; one combined XCTest run takes ~25s):

- **`VixlPortTests`** — the full vixl_port guard rail (integer + fp + neon)
  live-assembled and run on both gaby tracks under both oracles. All three
  families link into one process, so the runner walks the combined registered
  set once under a summed coverage baseline (`ios_runner_all` in
  [`gaby_two_track_main.h`](../test/test_support/vixl_asm/harness/gaby_two_track_main.h)).
  See [`docs/testing.md`](testing.md) for the harness itself.
- **`BaselineSuiteTests`** — the baseline/unit suites (`simulator_correctness`,
  `typed_register_io`, `branch_hook_*`, `shadow_runner`, …), each the same
  source as its host CTest executable.
- **`SmokeTests`** — link + public-API smokes.

Three host suites are **not** ported: `typed_register_io_abort`,
`simulator_constructor_stack`, and `predecode_cache_data_in_stream`. Each
`fork()`s a child to observe a deliberate abort, which the iOS sandbox forbids
(it works on the Simulator, which runs on the macOS host, but not on device).
They stay host/CTest-only; the data-in-stream literal-load behaviour is also
exercised by the vixl_port suite on iOS.

## Running the benchmark

The business microkernels run report-only via the `BenchTests` class:

```sh
xcodebuild test -project ios-runner/GabyRunner.xcodeproj -scheme GabyRunner \
  -destination 'platform=iOS Simulator,name=iPhone 16' \
  -only-testing:GabyRunnerTests/BenchTests
```

It runs each kernel in **cache** then **decoder** mode and prints the harness's
`key: value` report. There is **no native baseline on iOS** — that needs JIT /
executable memory — so the numbers are cache vs decoder (the real on-device
interpreter speed), without the slowdown-vs-native denominator. That denominator
stays a macOS-arm64 measurement; see [`bench/README.md`](../bench/README.md).

## On a physical device (local)

Open `ios-runner/GabyRunner.xcodeproj` in Xcode, select your connected iPhone,
and run the test action (⌘U) for the suites, or run `BenchTests` for on-device
numbers. Device builds need a development team: set it on the `GabyRunner` and
`GabyRunnerTests` targets (Signing & Capabilities → your team), or via a local,
untracked `xcconfig`. No team or signing identity is committed to the repo, and
simulator builds need none.

## Build option

Everything above is gated by the CMake option **`GABY_VM_BUILD_IOS_RUNNER`**
(default OFF). When ON it builds the vixl_port family libraries, the
baseline-suite libraries, and the business-bench library — but none of the
CTest executables. `generate.sh` passes it; the host CMake build and the
embedder are unaffected.

## In CI

See [`docs/refs/ci.md`](refs/ci.md) for the full CI map. In short:

- **`ci/ios-test.sh`** — a deterministic gate in `ci.yml`: build the runner, run
  the correctness suites on an arm64 Simulator (skipping `BenchTests`). A test
  failure fails the PR.
- **`ci/ios-bench.sh`** — report-only in `bench.yml` (push to main + dispatch):
  runs `BenchTests` and reports the numbers; never gates.

Both scripts run only where an arm64 iOS Simulator exists. On any other host
(e.g. an x86 runner) they **skip loudly** — reporting "did not run / skipped",
never a silent pass — and exit 0.
