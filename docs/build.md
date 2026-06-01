# Build system

User-facing build instructions (presets, demo binaries, embedding from a
parent CMake project) live in the repository root
[`../README.md`](../README.md). This document covers the **internal
structure** of the build — what's worth understanding when you change
`CMakeLists.txt` or add a target.

## Toolchain

- CMake ≥ 3.21 (presets file uses the 3.25 schema).
- C++17, `CMAKE_CXX_STANDARD 17` with `CMAKE_CXX_STANDARD_REQUIRED ON` and
  `CMAKE_CXX_EXTENSIONS OFF`.
- Compilers: GCC, Clang, AppleClang. MSVC isn't wired up today — Windows is
  out of scope per [`../AGENTS.md`](../AGENTS.md), and
  [`../cmake/CompileFlags.cmake`](../cmake/CompileFlags.cmake) gates flag
  application on `GNU|Clang|AppleClang`.

## Top-level options

- `GABY_VM_BUILD_TESTS` — defaults to `${PROJECT_IS_TOP_LEVEL}`. Off when
  consumed via `add_subdirectory`.
- `GABY_VM_BUILD_DEMOS` — same default.

## Targets

- `gaby_vm` (static library) plus the public alias `gaby_vm::gaby_vm`.
  Public include directory is `Sources/gaby_vm/include/`; private include
  directory is `Sources/gaby_vm/src/`.
  Compile features: `PUBLIC cxx_std_17`.
- Test executables under `test/`:
  `gaby_vm_smoke` (library sanity), `simulator_smoke` (Simulator + single
  NOP), `simulator_correctness` (hand-encoded sequences). Each links
  `PRIVATE gaby_vm::gaby_vm`; the two simulator tests additionally take a
  `PRIVATE` include of `${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src` and the
  VIXL compile defines (see "VIXL build defines" below) — this is the
  [privileged build pattern](#privileged-test-build-pattern).
- `gaby-vm` (CLI demo) under `demos/cli/`.

## VIXL build defines

Defined `PRIVATE` to `gaby_vm`, so they do **not** propagate via `INTERFACE`
to consumers:

- `VIXL_INCLUDE_TARGET_A64`
- `VIXL_INCLUDE_SIMULATOR_AARCH64`
- `VIXL_DEBUG` — only when `CMAKE_BUILD_TYPE` is `Debug`
  (via `$<$<CONFIG:Debug>:VIXL_DEBUG>`).

These defines aren't currently set, and the code they would gate isn't
imported: `VIXL_INCLUDE_TARGET_A32`, `VIXL_INCLUDE_TARGET_A32_T32`,
`VIXL_CODE_BUFFER_MMAP`, `VIXL_CODE_BUFFER_MALLOC`. If a later capability
needs any of them, both the define and the corresponding sources land
together.

## Warning-policy split

Two helpers in [`../cmake/CompileFlags.cmake`](../cmake/CompileFlags.cmake):

- `gaby_vm_apply_compile_flags(<target>)` — project policy. Adds
  `-Wall -Wextra -Wpedantic` to the target. Applied to `gaby_vm` itself and
  to every project-authored test/demo target.
- `gaby_vm_apply_imported_compile_flags(<file>...)` — imported-source
  relaxation. Operates on a **file list**, not a target, via
  `set_source_files_properties`. Adds `-Wall -Wextra` plus a list of
  documented `-Wno-*` flags. `-Wpedantic` is left out here because upstream
  VIXL code legitimately trips pedantic warnings. Each `-Wno-*` flag should
  carry an inline comment naming the warning class it suppresses, so the
  reason a warning is being silenced stays visible at the point of
  suppression.

The file-scoped relaxation means imported and project-authored sources can
coexist in the same target (`gaby_vm`) under different flag sets.

## Privileged test build pattern

`simulator_smoke` and `simulator_correctness` need to reach `vixl::aarch64`
types and headers that are not part of the public API. They get them via:

```cmake
target_include_directories(<target> PRIVATE ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src)
target_compile_definitions(<target> PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)
gaby_vm_apply_compile_flags(<target>)  # project policy, NOT the imported relaxation
```

New tests that need imported headers can follow the same pattern. Tests that
only exercise the public API (like `gaby_vm_smoke`) are better off without
the privilege — that keeps the default surface honest and catches accidental
public-API regressions.

## Presets

[`../CMakePresets.json`](../CMakePresets.json) defines `dev-debug` and
`dev-release` configure/build presets (both Ninja, both setting
`CMAKE_EXPORT_COMPILE_COMMANDS=ON` and enabling tests + demos). Only
`dev-debug` is wired as a test preset today; `dev-release` tests are run as
`ctest --test-dir build/release` until a release test preset is added.
