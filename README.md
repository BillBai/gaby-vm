# Gaby-VM

> **gaby-vm** — *Gaby's AArch64 BinarY interpreter*: an embeddable, JIT-free
> Arm CPU interpreter based on VIXL simulator semantics.

Gaby-VM is an embeddable Arm CPU instruction interpreter built on VIXL's
AArch64 simulator semantics. It ships as a standalone project rather
than an in-place fork of VIXL, and starts with AArch64 user-mode
execution as its first target.

It executes by predecoding each instruction once and caching the dispatch
target, then running the cached path repeatedly. There is no JIT and no
executable memory allocation, which is what lets Gaby-VM embed cleanly in
environments where runtime code generation isn't allowed — iOS being the
canonical example.

## Building

Requirements: CMake 3.21+, Ninja, and a C++17 compiler (GCC, Clang, or
AppleClang). MSVC is not supported.

Configure, build, and test using the bundled presets:

```sh
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

A `dev-release` preset is also available. Build outputs land in `build/debug/`
and `build/release/` respectively.

### Demo

The `demos/cli/` target produces a small CLI binary named `gaby-vm` that
embeds the static library — the macOS/Linux integration example. From the
debug build:

```sh
./build/debug/demos/cli/gaby-vm           # runs the embedding demo (prints version + host_add result)
./build/debug/demos/cli/gaby-vm --version # version only
./build/debug/demos/cli/gaby-vm --help    # usage
```

gaby-vm's correctness suites and its business benchmark also run on iOS — the
Simulator in CI, a physical device locally — through the committed Xcode host
under `ios-runner/`. Generate it with `ios-runner/generate.sh`; see
[`docs/ios.md`](docs/ios.md).

### Embedding

To consume gaby-vm from a parent CMake project, add it as a subdirectory and
link against the public alias target:

```cmake
add_subdirectory(third_party/gaby-vm)
target_link_libraries(your_target PRIVATE gaby_vm::gaby_vm)
```

Tests and demos default off when consumed via `add_subdirectory`. Override
explicitly with `-DGABY_VM_BUILD_TESTS=ON` or `-DGABY_VM_BUILD_DEMOS=ON` when
desired.

### SwiftPM

gaby-vm also ships a top-level `Package.swift`, so a Swift Package Manager
project can depend on it directly. This is aimed at C++ SPM consumers (the
primary embedder builds with SwiftPM); CMake stays the canonical build that
runs the tests and benchmarks. Both build systems compile the same sources.

Add gaby-vm as a package dependency and depend on the `gaby_vm` product:

```swift
// In your Package.swift
let package = Package(
    name: "MyHotfixEngine",
    dependencies: [
        .package(url: "https://example.invalid/gaby-vm.git", branch: "main"),
    ],
    targets: [
        .target(name: "MyHotfixEngine", dependencies: [
            .product(name: "gaby_vm", package: "gaby-vm"),
        ]),
    ],
    // Required: gaby-vm's public headers use C++20 (std::span, std::variant).
    // `cxxLanguageStandard` is per-package and does NOT cross the dependency
    // boundary, so the consumer must set it too — gaby-vm's own `.cxx20` only
    // applies to gaby-vm's targets.
    cxxLanguageStandard: .cxx20
)
```

Consume the public API from C++ with `#include "gaby_vm/…"` — no module map and
no Swift/C++ interop are required (the embedder is a C++ target). The consuming
package MUST compile at C++20 or later (set `cxxLanguageStandard: .cxx20` as
shown above): the public headers use `std::span` and `std::variant`, and a
package's C++ standard does not inherit from its dependencies. The product is a
static library, mirroring the CMake `gaby_vm` library. macOS and iOS are both
supported; the consuming app's deployment target is the effective floor.

A runnable macOS demo ships in the package. From the repository root:

```sh
swift run gaby_vm_spm_demo   # builds the library, links a C++ demo, prints version + host_add result
```

The demo is an executable target, not a library product, so projects that
depend on the `gaby_vm` product never build it.
