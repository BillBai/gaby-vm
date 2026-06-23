# ios-runner Specification

## Purpose
TBD - created by archiving change add-ios-support. Update Purpose after archive.
## Requirements
### Requirement: An iOS runner executes the correctness suites and benchmark harness on iOS

The repository SHALL contain a committed Xcode project under `ios-runner/`
with a host application target and an XCTest target. The XCTest target SHALL
execute the same correctness suites that the host CTest suite runs, reporting
per-suite pass/fail. The runner SHALL also be able to execute the benchmark
harness. The runner SHALL build and run for both the iOS Simulator (arm64) and
a physical iOS device (arm64).

#### Scenario: Correctness suites run on the Simulator

- **WHEN** the runner's XCTest target runs on an arm64 iOS Simulator
- **THEN** each correctness suite executes and reports pass/fail per suite

#### Scenario: Correctness suites run on a device

- **WHEN** a developer selects a connected iPhone in Xcode and runs the test action
- **THEN** the same correctness suites execute on the device

#### Scenario: Benchmark harness runs on iOS

- **WHEN** the runner executes the benchmark harness
- **THEN** it produces the harness's key/value report for each kernel

### Requirement: The runner consumes the library without forking its source list

The runner SHALL obtain the `gaby_vm` library either by referencing a
CMake-generated Xcode project for the `gaby_vm` target and depending on its
library product, OR, as a fallback, by compiling the gaby_vm sources directly
under a guard that fails the build when the runner's source list diverges from
CMake's `GABY_VM_IMPORTED_SOURCES` / `GABY_VM_SOURCES`. The runner SHALL NOT
integrate a prebuilt static library.

#### Scenario: Library built per destination via the generated subproject

- **WHEN** the runner builds via the CMake-generated subproject for a selected destination
- **THEN** it links the `gaby_vm` product CMake built for that destination, with no second copy of the source list

#### Scenario: Drift guard catches a diverged source list

- **WHEN** the fallback direct-compile path is in use and a source file is added to CMake's lists but not to the runner
- **THEN** the drift guard fails the build

#### Scenario: No prebuilt static library is integrated

- **WHEN** the runner's link inputs are inspected
- **THEN** they contain no committed or pre-staged `.a`; the library is built from source as part of the runner build

### Requirement: Correctness suites share one implementation across host and iOS

Each correctness suite SHALL be invocable through a callable entry point that
both the host CTest executable and the iOS XCTest case call. Suite logic SHALL
NOT be duplicated for iOS.

#### Scenario: One change, both drivers

- **WHEN** a suite's entry point is changed
- **THEN** both the host CTest run and the iOS XCTest run exercise the change without a separate iOS edit

### Requirement: The runner is a development build with no committed signing identity

Simulator builds of the runner SHALL build and run without a code-signing
identity. Device builds SHALL take development signing from local developer
configuration (for example an untracked xcconfig or automatic signing with the
developer's team). The repository SHALL NOT hardcode a specific signing
identity or development team.

#### Scenario: Simulator build needs no signing

- **WHEN** the runner is built for the iOS Simulator
- **THEN** it builds with code signing disabled and requires no developer team

#### Scenario: Device signing comes from local configuration

- **WHEN** the runner is built for a device
- **THEN** signing is supplied by local developer configuration, and no specific team or identity is committed to the repository

