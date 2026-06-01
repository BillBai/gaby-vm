// swift-tools-version:5.9
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// SwiftPM manifest for gaby-vm. This is a distribution channel for C++ SPM
// consumers (the primary embedder builds with SwiftPM); CMake remains the
// canonical development, test, and benchmark build. Both build systems compile
// the same sources under Sources/gaby_vm/ — see openspec change
// `swiftpm-package` and docs/build.md.

import PackageDescription

let package = Package(
    name: "gaby_vm",
    // macOS and iOS are both supported. The minimums are intentionally
    // permissive: the consuming embedder app's deployment target is the
    // effective floor. Raise these only if a C++20 libc++ feature used by the
    // public headers (std::span / std::variant / std::function) turns out to
    // carry an availability annotation that bites on an older OS — see
    // design.md Decision 4.
    platforms: [
        .macOS(.v10_15),
        .iOS(.v13),
    ],
    products: [
        // A single static library product, mirroring the CMake `gaby_vm`
        // STATIC library. This is the only library product; the macOS demo
        // (added below) is an executable target, not a product, so consumers
        // that depend on `gaby_vm` never build it.
        .library(name: "gaby_vm", type: .static, targets: ["gaby_vm"]),
    ],
    targets: [
        .target(
            name: "gaby_vm",
            path: "Sources/gaby_vm",
            // The CMake build compiles an explicit source list; SwiftPM
            // compiles every source under the target path. The two agree
            // because only the imported VIXL .cc files and the project-authored
            // gaby_vm/*.cc live under src/. CMakeLists.txt is the lone
            // non-source file SwiftPM would otherwise flag as unhandled.
            exclude: ["src/CMakeLists.txt"],
            // Public API: the six gaby_vm/*.h headers, consumed as
            // #include "gaby_vm/<name>.h". Resolves to Sources/gaby_vm/include.
            // Imported VIXL headers stay under src/ and are NOT public.
            publicHeadersPath: "include",
            // Reproduce the CMake target's PRIVATE compile environment exactly.
            // Without these the imported VIXL code does not compile correctly.
            // The defines are target-scoped (SPM has no INTERFACE-define
            // leakage), matching CMake's PRIVATE scoping — consumers never see
            // VIXL_*. Out-of-scope VIXL defines (VIXL_INCLUDE_TARGET_A32*,
            // VIXL_CODE_BUFFER_*, VIXL_HAS_SIMULATED_MMAP) are deliberately
            // left unset, exactly as in CMake.
            cxxSettings: [
                .headerSearchPath("src"),  // CMake PRIVATE src/
                .define("VIXL_INCLUDE_TARGET_A64"),
                .define("VIXL_INCLUDE_SIMULATOR_AARCH64"),
                .define("VIXL_DEBUG", .when(configuration: .debug)),
            ]
        ),
        // macOS verification demo. A C++ executable that consumes the gaby_vm
        // product through its public headers (#include "gaby_vm/…") and runs a
        // guest program through gaby_vm::Simulator. `swift run gaby_vm_spm_demo`
        // proves the SPM build closes and is usable. It depends only on the
        // public API, so it carries none of the VIXL defines or the private
        // src/ header path — those stay scoped to the library target. As an
        // executable target (not a product), it is never built by consumers of
        // the gaby_vm library.
        .executableTarget(
            name: "gaby_vm_spm_demo",
            dependencies: ["gaby_vm"],
            path: "demos/spm"
        ),
    ],
    cxxLanguageStandard: .cxx20
)
