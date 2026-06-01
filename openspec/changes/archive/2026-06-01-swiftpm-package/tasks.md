## 1. Baseline (regression reference)

- [x] 1.1 Create a feature branch off `main` for the change.
- [x] 1.2 Record the green baseline the move must preserve: `cmake --preset dev-debug`, `cmake --build --preset dev-debug`, `ctest --preset dev-debug` all pass at HEAD. The existing CTest suite is the regression guard for the relocation.

## 2. Header-only version() (spec: "version() is header-only and the single source of truth")

- [x] 2.1 In `include/gaby_vm/version.h`, define `GABY_VM_VERSION_STRING` and make `version()` an `inline` function returning it (single source of truth).
- [x] 2.2 Delete `src/version.cc.in`; remove the `configure_file` and `target_sources(... version.cc)` lines from `src/CMakeLists.txt`; drop `VERSION` from the root `project()` clause.
- [x] 2.3 Verify: CMake configures with no project version, builds, and `ctest` passes; confirm no `version.cc` is generated and `version()` returns the macro string (existing version test / demo `--version`).

## 3. Structural move to `Sources/gaby_vm/` (spec: "Sources and public headers live under Sources/gaby_vm/", "CMake remains the canonical build")

- [x] 3.1 `git mv include Sources/gaby_vm/include` and `git mv src Sources/gaby_vm/src`, preserving internal trees.
- [x] 3.2 Re-root the root `CMakeLists.txt` `add_subdirectory` to `Sources/gaby_vm/src`; update `target_include_directories` PUBLIC/PRIVATE paths in the src CMakeLists; confirm `test/`, `bench/`, `demos/` CMake references resolve via the target, not hardcoded `src/`/`include/` paths.
- [x] 3.3 Sweep doc + tooling path references to the new `Sources/gaby_vm/...` locations: `docs/refs/vixl-extraction-map.md`, `docs/architecture.md`, `docs/build.md`, `docs/conventions.md`, per-dir `.clangd` / `.clang-format` (grep for `"/src/"` and `"/include/"`).
- [x] 3.4 Verify CMake green: re-run configure/build/ctest = the §1 baseline; confirm no path-reference breakage and extraction-map references resolve to `Sources/gaby_vm/src/`.

## 4. Package.swift manifest (spec: "single static library product", "reproduces the CMake private compile environment", "declares and is verified to build for macOS and iOS")

- [x] 4.1 Add root `Package.swift`: `swift-tools-version:5.9`, `cxxLanguageStandard: .cxx20`, one `gaby_vm` target with `path: "Sources/gaby_vm"` and `publicHeadersPath: "include"`, product `.library(name: "gaby_vm", type: .static, targets: ["gaby_vm"])`, platforms declaring macOS + iOS with no restrictive minimum.
- [x] 4.2 In the target `cxxSettings`, set `.headerSearchPath("src")` and the three defines `VIXL_INCLUDE_TARGET_A64`, `VIXL_INCLUDE_SIMULATOR_AARCH64`, and `VIXL_DEBUG` (debug-only); leave the out-of-scope VIXL defines unset.
- [x] 4.3 Add `.build/`, `.swiftpm/`, `Package.resolved` to `.gitignore`.
- [x] 4.4 Verify: `swift build` succeeds on macOS and produces the `gaby_vm` static archive; confirm public headers resolve via `#include "gaby_vm/..."` and imported VIXL headers are not on the public path.

## 5. macOS SPM demo / closure check (spec: "A macOS SPM demo proves the build closes and is usable", "No custom module map and no Swift-interop dependency")

- [x] 5.1 Add a C++ `.executableTarget` (sources e.g. `demos/spm/`) depending on the `gaby_vm` product that calls `version()` and runs a small guest program through a `gaby_vm::Simulator`; do not export it as a library product; ship no `module.modulemap` and use no Swift interop.
- [x] 5.2 Verify: `swift run <demo>` builds, links, executes, and prints the version + guest result; confirm no custom `module.modulemap` exists and the demo target is not a library product.

## 6. iOS build compatibility (spec: "the library SHALL compile and link for the iOS SDK")

- [x] 6.1 Verify the library builds for iOS: `xcodebuild -scheme gaby_vm -destination 'generic/platform=iOS Simulator' build` compiles and links (build-only — no app bundle, no signing, no run). Optionally repeat for the device SDK.

## 7. README + CI

- [x] 7.1 Add a `### SwiftPM` subsection under README "### Embedding" with the SPM consumer snippet and the macOS `swift run` demo note.
- [ ] 7.2 Add CI steps on a macOS host alongside the CMake job: `swift run` (macOS demo) and the iOS `xcodebuild` library build; skipped where `swift`/Xcode are unavailable.
  - DEFERRED (follow-up): the repository has no CI infrastructure yet (no `.github/workflows/`, no other CI config), so there is no existing CMake job to add these steps "alongside." Per the change owner's call, CI wiring is split into a separate follow-up rather than bootstrapping a full pipeline inside this change. All four checks this task would gate are verified locally and green (CMake build/ctest, `swift build`, `swift run gaby_vm_spm_demo`, iOS `xcodebuild`).

## 8. Final verification

- [x] 8.1 Full green sweep: CMake configure/build/ctest, `swift build`, the `swift run` demo, and the iOS `xcodebuild` build all pass; `openspec validate swiftpm-package` reports valid.
