## Why

The project's primary embedder is a C++ user-mode hot-fix engine whose
own build chain is Swift Package Manager (SPM). Today gaby-vm only ships
a CMake build — embedders consume it via `add_subdirectory` from a parent
CMake project. An SPM-native consumer either drops CMake entirely (not
viable; CMake is how gaby-vm is developed, tested, and benchmarked) or
hand-maintains an SPM shim that mirrors the CMake target shape and drifts
on every gaby-vm change.

A committed top-level `Package.swift` lets SPM consumers pull gaby-vm in
directly — `dependencies: [.package(url: ".../gaby-vm", ...)]` — without
going through CMake. CMake stays the canonical build for development,
testing, and benchmarking; SPM is the distribution channel for the C++
embedder.

The change was originally sequenced behind the sibling `branch-hook-api`
change because `Package.swift` references public headers whose surface
was in flux. `branch-hook-api` is now complete (merged in `261a22b`), so
the six public headers under `include/gaby_vm/` are settled and this
change is unblocked.

iOS and macOS are both in scope. The embedder is a library that itself
supports iOS, so gaby-vm must build for iOS too. The imported simulator
has no Apple-specific code paths and no executable-memory / `mmap` path
on Apple (its only host-syscall path is `__linux__`-gated), so —
consistent with the project's no-JIT charter — iOS support here is
**build compatibility** (the library compiles and links for the iOS
SDK), verified in CI, not porting. There is no iOS app or UI demo in this
change.

## What Changes

- **Adopt SPM's folder layout (structural move).** Relocate the existing
  source and header trees under one SPM-conventional parent, internal
  structure unchanged:
  - `include/gaby_vm/` → `Sources/gaby_vm/include/gaby_vm/`
  - `src/` → `Sources/gaby_vm/src/`

  This is required because a SwiftPM target is rooted at a single
  directory and `publicHeadersPath` is relative to it — the current
  sibling `src/` + `include/` cannot both belong to one target without
  rooting the target at the repository (`path: "."`, with a fragile
  exclude list). See `design.md` Decision 1.

- **Add `Package.swift`** at the repository root declaring a single
  static C++ library product `gaby_vm` that mirrors the CMake `gaby_vm`
  static library:
  - `.library(name: "gaby_vm", type: .static, targets: ["gaby_vm"])`.
  - `cxxLanguageStandard: .cxx20` (matches the CMake build).
  - One target `path: "Sources/gaby_vm"`, `publicHeadersPath: "include"`.
  - `cxxSettings` reproduce the CMake target's private compile
    environment — **required**, not optional, or the imported VIXL code
    will not compile correctly:
    - `.headerSearchPath("src")` (CMake's `PRIVATE src/`).
    - `.define("VIXL_INCLUDE_TARGET_A64")`,
      `.define("VIXL_INCLUDE_SIMULATOR_AARCH64")`,
      `.define("VIXL_DEBUG", .when(configuration: .debug))`.
  - Declares iOS + macOS support with no restrictive minimum; the
    embedder app's deployment target is the effective floor.

- **Make `version()` header-only, single source of truth.** Replace the
  CMake-generated `version.cc` with an `inline` definition in
  `include/gaby_vm/version.h` backed by a `GABY_VM_VERSION_STRING` macro.
  Delete `src/version.cc.in` and the `configure_file` /
  `target_sources(... version.cc)` lines in the CMakeLists. Drop `VERSION`
  from the root `project(gaby_vm …)` clause (`@PROJECT_VERSION@` in
  `version.cc.in` was its only consumer).

- **No module map.** A C++ SPM dependent consumes the library through
  `publicHeadersPath` and `#include "gaby_vm/…"`; no `module.modulemap`
  is needed. A Swift `import gaby_vm` story is out of scope.

- **Verification = a macOS SPM demo + an iOS library build:**
  - *macOS:* a small C++ `.executableTarget` in `Package.swift` (sources
    e.g. under `demos/`) consumes the `gaby_vm` product, calls
    `version()`, and runs a tiny guest program. `swift run` building and
    executing it proves the SPM build closes and is usable on macOS, and
    serves as the macOS demo. It is a dev/demo target, not part of the
    library product, so consumers never build it.
  - *iOS:* CI builds the `gaby_vm` **library** for the iOS SDK via
    `xcodebuild` (Simulator, optionally device) — compile + link only, no
    app, no signing, no run. This is the "iOS is in scope" guarantee: the
    library is iOS-buildable, verified.

  Neither check links gaby-vm into a Swift executable or ships an iOS app
  — Swift/C++ interop and an iOS UI demo are out of scope.

- **README addition:** a `### SwiftPM` subsection under the existing
  "### Embedding" header showing the SPM consumer snippet:
  ```swift
  .package(url: "https://example.invalid/gaby-vm.git", branch: "main"),
  // ...
  .target(name: "MyHotfixEngine", dependencies: [
      .product(name: "gaby_vm", package: "gaby-vm"),
  ]),
  ```

- **`.gitignore`:** add SPM's transient artefacts (`.build/`,
  `.swiftpm/`, `Package.resolved`).

- **No change to C++ leaf semantics or the public API surface.** Files
  move; their contents do not, except `version.h` (header-only
  `version()`) and the deleted `version.cc.in`.

## Capabilities

### New Capabilities

- `swiftpm-package`: the contract for what `swift build` against this
  repository produces — which sources are included, which headers are
  public, which platforms are declared and **verified to build** (macOS
  via `swift run`, iOS via an `xcodebuild` library build), the private
  compile environment (VIXL defines + `src/` header search path) the SPM
  target reproduces from CMake, and the relationship between the SPM and
  CMake build outputs (same static library, same sources).

### Modified Capabilities
<!--
No existing capability's *behaviour* changes — this is a new distribution
channel alongside CMake, and the structural move plus the header-only
version() are build-mechanics changes.

However, the relocation does invalidate the `src/` and `include/gaby_vm/`
path references that the `aarch64-simulator` and `benchmark-harness` specs
pin normatively (e.g. "imported VIXL files SHALL be placed … under `src/`").
Those path references were re-rooted to `Sources/gaby_vm/src/` and
`Sources/gaby_vm/include/gaby_vm/` directly in the main specs
(`openspec/specs/aarch64-simulator/spec.md`,
`openspec/specs/benchmark-harness/spec.md`) — a mechanical path rename, not a
behaviour delta, so it was applied in place rather than carried as MODIFIED
delta specs in this change. Upstream `../vixl/src/` citations are unchanged.
-->

## Impact

- **Source layout (changed).** `src/` and `include/gaby_vm/` move under
  `Sources/gaby_vm/`. The internal tree of `src/` is preserved (only its
  path prefix changes), so the VIXL extraction map's relationships stay
  intact — its `src/…` references re-root to `Sources/gaby_vm/src/…`.
- **Build system.** `Package.swift` lives alongside `CMakeLists.txt` at
  the repository root. The two builds compile the same sources from the
  new layout; CMake stays canonical (it runs tests and benchmarks), SPM
  is for downstream C++ embedders.
- **CMake (changed).** Re-root `add_subdirectory(src)` and the target's
  include directories to the new paths; delete the `version.cc`
  generation; drop `project(... VERSION ...)`. The gating check for this
  change is that **CMake still builds green** after the relocation.
- **Docs / tooling (changed).** `docs/refs/vixl-extraction-map.md`,
  `docs/architecture.md`, `docs/build.md`, `docs/conventions.md`, and the
  per-directory `.clangd` / `.clang-format` references update to the new
  paths.
- **Specs (changed).** The `aarch64-simulator` and `benchmark-harness` specs
  pin `src/` and `include/gaby_vm/` paths normatively; those references are
  re-rooted in place to `Sources/gaby_vm/...` (see Modified Capabilities
  above). Upstream `../vixl/src/` citations are left unchanged.
- **CI.** New checks alongside the CMake test job, on macOS hosts:
  `swift run` for the macOS SPM demo, and an `xcodebuild` library build
  for the iOS SDK (build-only). Skipped where `swift` / Xcode are
  unavailable (e.g. Linux runners). No `swift test` (the package has no
  Swift tests).
- **Public API.** Untouched. SPM consumers see the same
  `gaby_vm::Simulator`, `PredecodeCache`, `RegisterFile`, etc. as CMake
  consumers.
- **Versioning.** `include/gaby_vm/version.h` becomes the single source
  of truth. A tagged release strategy (`git tag v0.x.y`) is implied;
  defining the release cadence is not part of this change.
- **iOS.** This change verifies gaby-vm *compiles and links for iOS* (an
  `xcodebuild` library build), since the embedder is an iOS-supporting
  library — iOS support is a checked build guarantee. There is no iOS app
  or UI demo; a `demos/ios/` Xcode example (noted in the README) and any
  on-device run remain follow-up work.
- **Linux SPM.** Works in principle (SPM supports C++ targets on Linux)
  but is not a project test target. Best-effort.
- **Out of scope** (will not be touched):
  - Any change to C++ leaf semantics or the public API.
  - An iOS app, UI demo, or `demos/ios/` Xcode project; `.xcframework` /
    framework packaging. (iOS *library build* compatibility is in scope;
    a runnable iOS app is not.)
  - A Swift `import gaby_vm` interop layer / custom module map.
  - Per-file warning policy under SPM (CMake stays the strict gate).
