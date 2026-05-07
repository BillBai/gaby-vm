# Gaby-VM

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
./build/debug/demos/cli/gaby-vm           # banner with version
./build/debug/demos/cli/gaby-vm --version # version only
./build/debug/demos/cli/gaby-vm --help    # usage
```

An iOS Xcode demo will land at `demos/ios/` in a follow-up change.

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
