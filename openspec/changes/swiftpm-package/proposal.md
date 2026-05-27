## Why

The project's primary embedder is a C++ hot-fix engine whose own
build chain is Swift Package Manager (SPM). Today gaby-vm only ships
a CMake build — embedders are expected to consume it via
`add_subdirectory` from a parent CMake project. An SPM-native consumer
either has to drop CMake support entirely (not viable) or maintain a
hand-rolled SPM shim that mirrors the CMake target shape (fragile,
the two will drift on every gaby-vm change).

Adding a top-level `Package.swift` lets SPM consumers pull gaby-vm
in directly — `dependencies: [.package(url: ".../gaby-vm", ...)]` —
without going through CMake at all. The CMake build stays the
canonical one for development, testing, and benchmarking; SPM is the
distribution channel for the C++ embedder.

iOS is out of scope for this change (the project's iOS embedding work
is sequenced behind the hot-fix engine integration), but the SPM
package's layout — public headers under `include/gaby_vm/`, source
under `src/`, module map for C++ headers — is designed so that an
iOS Xcode-driven build can later add an Apple-platform target without
restructuring.

## What Changes

- Add `Package.swift` at the repository root, declaring a single
  C++ library product `gaby_vm` that mirrors the CMake `gaby_vm`
  static library:
  - `cxxLanguageStandard: .cxx17` (matches the CMake build).
  - `sources` covers `src/` recursively, excluding `src/CMakeLists.txt`.
  - `publicHeadersPath: "include"` exposes `include/gaby_vm/` as the
    public header search path.
  - Platform clause sets the supported platforms (macOS 13+ as the
    initial target; iOS / tvOS / watchOS are deliberately omitted
    from this change but can be appended later without rework).
- Add `include/gaby_vm/module.modulemap` defining a clang module
  `gaby_vm` that exports the six public headers
  (`gaby_vm.h`, `predecode_cache.h`, `registers.h`,
  `shadow_runner.h`, `simulator.h`, `version.h`). This is what lets a
  Swift consumer `import gaby_vm` if they ever want to — for the
  current pure-C++ embedder it is harmless but makes the package
  forward-compatible.
- Add a top-level `.spi.yml` and `.swiftpm/` directory ignore entries
  to `.gitignore` (SPM's transient artefacts: `.build/`,
  `Package.resolved` if any).
- Add a verification job to the test/CI pipeline (`test/spm/` or
  similar): a tiny driver that runs `swift build` against
  `Package.swift` and ensures the resulting `.a` exposes the same
  public symbols as the CMake `libgaby_vm.a`. The driver does NOT
  link gaby-vm into a Swift executable — that's iOS-flavored work
  scheduled separately. It only verifies the SPM build closes.
- README addition: a `### SwiftPM` subsection under the existing
  "Embedding" header showing the SPM consumer snippet:
  ```swift
  .package(url: "https://example.invalid/gaby-vm.git", branch: "main"),
  // ...
  .target(name: "MyHotfixEngine", dependencies: [
      .product(name: "gaby_vm", package: "gaby-vm"),
  ]),
  ```
- No C++ source changes. The existing `include/gaby_vm/` layout
  already matches SPM's `publicHeadersPath` convention; the SPM
  package consumes the same files the CMake build does.

## Capabilities

### New Capabilities

- `swiftpm-package`: the contract for what `swift build` against this
  repository produces — which sources are included, which headers
  are public, which platforms are declared, the relationship between
  the SPM and CMake build outputs (same static library shape, same
  public symbols).

### Modified Capabilities
<!-- None — this is a new distribution channel alongside CMake; no
existing capability's behaviour changes. -->

## Impact

- **Source layout**: no C++ files moved or renamed. SPM consumes the
  existing `src/` and `include/gaby_vm/` directories. The only new
  user-visible file is `include/gaby_vm/module.modulemap`.
- **Build system**: `Package.swift` lives alongside `CMakeLists.txt`
  at the repository root. The two builds are parallel — CMake stays
  the canonical development build (it's how tests and benchmarks are
  run); SPM is for downstream C++ embedders that use SPM.
- **CMake**: unchanged. The CMake configuration does not need to know
  about SPM.
- **CI**: a new lightweight `swift build` check runs alongside the
  existing CMake test job. Skipped on Linux runners that don't have
  `swift` available; on macOS CI hosts, runs as a build-only check
  (no `swift test`, since the package has no Swift tests).
- **Public API**: untouched. SPM consumers see the same
  `gaby_vm::Simulator`, `PredecodeCache`, `RegisterFile`, etc. as
  CMake consumers do.
- **README**: a new "SwiftPM" subsection. The existing CMake
  embedding instructions stay.
- **Versioning**: SPM's package version follows the existing
  `include/gaby_vm/version.h` convention. A tagged release strategy
  (`git tag v0.x.y`) is implied but defining the release cadence is
  not part of this change.
- **iOS, tvOS, watchOS, visionOS targets**: explicitly **not** added
  by this change. Apple platforms beyond macOS need linker and
  conditional-compilation work that doesn't exist yet; layering them
  in is a separate change once the hot-fix engine integration is
  unblocked.
- **Linux SPM**: works in principle (SPM on Linux supports C++
  targets) but isn't a project test target. Verification on Linux is
  best-effort.
- **Out of scope** for this change (will not be touched):
  - Any C++ code change.
  - iOS Xcode project / framework / `.xcframework` packaging.
  - Branch hook API — that's the sibling change `branch-hook-api`,
    which should land first since the proposal of `Package.swift`
    will reference public headers whose surface area is in flux
    until `branch-hook-api` ships.
