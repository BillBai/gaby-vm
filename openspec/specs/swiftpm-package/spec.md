# swiftpm-package Specification

## Purpose
Ship Gaby-VM as a Swift Package Manager package alongside the canonical CMake
build, so embedders on Apple platforms can consume the `gaby_vm` static library
through SwiftPM. This capability owns the top-level `Package.swift` manifest,
the `Sources/gaby_vm/` source/header layout, the SPM-side reproduction of the
CMake private compile environment, the header-only `version()` source of truth,
the macOS demo that proves consumption closes, and the guarantee that CMake
remains the canonical, strict-warning build after the source relocation.

## Requirements

### Requirement: Repository ships a top-level SwiftPM manifest with one static library product

The repository SHALL contain a `Package.swift` at its root that declares a
single library product named `gaby_vm`, built from one target also named
`gaby_vm`.

The product SHALL be a static library — `.library(name: "gaby_vm", type:
.static, targets: ["gaby_vm"])` — so it mirrors the CMake `gaby_vm`
`STATIC` library and links statically into a consuming SPM target.

The manifest SHALL set `cxxLanguageStandard: .cxx20`, matching the CMake
build's `cxx_std_20`. The declared `swift-tools-version` SHALL be high
enough to support C++ targets and `.cxx20` (5.9 or later).

`gaby_vm` SHALL be the only *library* product. Any demo or verification
executable (see the macOS demo requirement) SHALL be a separate target
that is NOT exported as a library product, so package consumers that
depend on the `gaby_vm` product do not build it.

#### Scenario: `swift build` produces the gaby_vm static library

- **WHEN** `swift build` is run at the repository root on a macOS host
- **THEN** the build succeeds
- **AND** it produces a static archive for the `gaby_vm` target

#### Scenario: The only library product is the static gaby_vm library

- **WHEN** the `products` array in `Package.swift` is inspected
- **THEN** exactly one `.library` product exists, named `gaby_vm`, of type `.static`
- **AND** any executable target (demo / verification) is not listed as a library product

### Requirement: Sources and public headers live under `Sources/gaby_vm/`

The SwiftPM target SHALL be rooted at `Sources/gaby_vm/`. The C++ sources
SHALL live under `Sources/gaby_vm/src/` (the imported VIXL tree and the
project-authored `gaby_vm/*.cc`), preserving the internal structure of
the former top-level `src/`. The six public headers SHALL live under
`Sources/gaby_vm/include/gaby_vm/`, with `publicHeadersPath: "include"`.

The public header surface exposed to consumers SHALL be exactly the six
headers `gaby_vm.h`, `predecode_cache.h`, `registers.h`,
`shadow_runner.h`, `simulator.h`, and `version.h`, reachable as
`#include "gaby_vm/<name>.h"`.

The imported VIXL private headers (e.g. `simulator-aarch64.h`,
`utils-vixl.h`, the `aarch64/*.h` set) SHALL NOT be part of the public
header surface; they live under `Sources/gaby_vm/src/`, outside
`publicHeadersPath`.

#### Scenario: A consumer includes the public API with the gaby_vm/ prefix

- **WHEN** a C++ consumer of the `gaby_vm` product compiles a translation unit containing `#include "gaby_vm/simulator.h"`
- **THEN** the header resolves through the package's public headers path
- **AND** no `vixl::*` symbol or imported-header path is required to use it

#### Scenario: Imported VIXL headers are not exposed as public

- **WHEN** the package's `publicHeadersPath` contents are inspected
- **THEN** only the six `gaby_vm/*.h` headers are present
- **AND** imported VIXL headers under `Sources/gaby_vm/src/` are not reachable via the public include path

### Requirement: The SPM target reproduces the CMake private compile environment

The SwiftPM target SHALL set, via `cxxSettings`, the same private compile
configuration the CMake target carries, without which the imported VIXL
sources do not compile correctly:

- `.define("VIXL_INCLUDE_TARGET_A64")`
- `.define("VIXL_INCLUDE_SIMULATOR_AARCH64")`
- `.define("VIXL_DEBUG", .when(configuration: .debug))`
- `.headerSearchPath("src")` — the private header search path equivalent
  to the CMake target's `PRIVATE src/`.

These defines SHALL be scoped to the target only and SHALL NOT propagate
to consumers, matching the CMake `PRIVATE` scoping. The out-of-scope VIXL
defines (`VIXL_INCLUDE_TARGET_A32*`, `VIXL_CODE_BUFFER_*`,
`VIXL_HAS_SIMULATED_MMAP`) SHALL NOT be set, matching the CMake build.

#### Scenario: Imported VIXL sources compile under the SPM build

- **WHEN** `swift build` compiles the imported `aarch64/simulator-aarch64.cc` and its siblings
- **THEN** the two `VIXL_INCLUDE_*` defines and the debug-only `VIXL_DEBUG` are in effect
- **AND** the private `src/` header search path resolves includes such as `aarch64/decoder-aarch64.h`

#### Scenario: Out-of-scope VIXL defines stay unset

- **WHEN** the target's `cxxSettings` are inspected
- **THEN** `VIXL_INCLUDE_TARGET_A32*`, `VIXL_CODE_BUFFER_*`, and `VIXL_HAS_SIMULATED_MMAP` are not defined

### Requirement: `version()` is header-only and the single source of truth

`gaby_vm::version()` SHALL be defined `inline` in
`include/gaby_vm/version.h`, returning a `GABY_VM_VERSION_STRING` macro
defined in that same header. That macro SHALL be the single source of
truth for the project version.

The build SHALL NOT generate a `version.cc` from a template. The
`src/version.cc.in` file and the CMake `configure_file` /
`target_sources(... version.cc)` lines SHALL be removed. The root
`project()` clause SHALL NOT declare a `VERSION` (its only former
consumer, `@PROJECT_VERSION@` in `version.cc.in`, no longer exists).

#### Scenario: version() resolves with no generated source

- **WHEN** any consumer includes `gaby_vm/version.h` and calls `gaby_vm::version()`
- **THEN** it returns the `GABY_VM_VERSION_STRING` value
- **AND** no `version.cc` is generated by either the SPM or CMake build

#### Scenario: CMake builds without a project version

- **WHEN** the CMake build is configured after the change
- **THEN** the root `project()` declares no `VERSION`
- **AND** the configure and build steps succeed

### Requirement: The package declares and is verified to build for macOS and iOS

The manifest SHALL declare both macOS and iOS as supported platforms. It
SHALL NOT impose a deployment-target minimum stricter than required by the
C++20 standard-library features the public headers use; the consuming
(embedder) app's deployment target governs the effective floor.

The `gaby_vm` library SHALL compile and link for the iOS SDK, verified by
an `xcodebuild` library build (iOS Simulator destination, and optionally
the device SDK). This is a build-only guarantee: no app bundle, no code
signing, no run.

#### Scenario: The library builds for macOS

- **WHEN** `swift build` is run at the repository root on a macOS host
- **THEN** the `gaby_vm` library compiles and links successfully

#### Scenario: The library builds for the iOS SDK

- **WHEN** `xcodebuild` builds the `gaby_vm` scheme with an iOS Simulator destination
- **THEN** the library compiles and links for iOS
- **AND** no app bundle, code signing, or run is required for the check to pass

### Requirement: A macOS SPM demo proves the build closes and is usable

The package SHALL include a C++ executable target that depends on the
`gaby_vm` product, calls `gaby_vm::version()`, and runs a small guest
program through a `gaby_vm::Simulator`. Running it with `swift run` SHALL
build, link, and execute it successfully, demonstrating SPM consumption on
macOS.

This executable SHALL be a dev/demo target, not part of the `gaby_vm`
library product, so consumers of the library never build it.

#### Scenario: `swift run` executes the macOS demo

- **WHEN** the demo target is run with `swift run` on a macOS host
- **THEN** it builds and links against the `gaby_vm` product
- **AND** it prints the version and the result of the guest program

### Requirement: No custom module map and no Swift-interop dependency

The package SHALL NOT ship a custom `module.modulemap`. Consumption SHALL
be through C++ `#include "gaby_vm/<name>.h"` against the package's public
headers path; it SHALL NOT depend on Swift `import gaby_vm` or
Swift/C++ interoperability mode.

#### Scenario: No custom module map is present

- **WHEN** the package's `publicHeadersPath` is inspected
- **THEN** no hand-authored `module.modulemap` file is present
- **AND** consuming the library requires no `.interoperabilityMode(.Cxx)` setting

### Requirement: CMake remains the canonical build and stays green after the move

The CMake build SHALL remain fully functional after the source
relocation under `Sources/gaby_vm/`: configuring, building, and running
the test suite MUST all succeed. CMake remains the canonical,
strict-warning build; the SPM package is an additional distribution
channel that does not replace it.

#### Scenario: CMake configures, builds, and tests after relocation

- **WHEN** the bundled CMake preset is configured, built, and `ctest` is run after the files move under `Sources/gaby_vm/`
- **THEN** all three steps succeed with no path-reference breakage
- **AND** the imported VIXL extraction-map references resolve to their new `Sources/gaby_vm/src/` locations
