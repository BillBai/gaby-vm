## Context

gaby-vm ships today as a CMake static library. The canonical consumption
path is `add_subdirectory(third_party/gaby-vm)` from a parent CMake
project, linking `gaby_vm::gaby_vm`. That works for CMake consumers and
stays the project's development, test, and benchmark build.

The project's primary embedder is a C++ user-mode hot-fix engine whose
own build chain is Swift Package Manager. An SPM consumer cannot pull a
CMake project in directly — it either drops CMake (not viable; CMake is
how we test and benchmark) or hand-maintains an SPM shim that mirrors the
CMake target shape and drifts on every gaby-vm change. A committed
top-level `Package.swift` removes that drift: SPM consumers depend on
gaby-vm the SPM way, CMake stays canonical, and the two builds compile
the same source.

Two things shifted since this change was first proposed:

1. **The sequencing blocker cleared.** The proposal deferred to the
   sibling `branch-hook-api` change because `Package.swift` references
   public headers whose surface was in flux. `branch-hook-api` is now
   complete (merged in `261a22b`); the six public headers under
   `include/gaby_vm/` are settled.

2. **iOS is in scope.** The original proposal scoped this to macOS and
   deferred Apple platforms. Inspection of the imported simulator shows
   the iOS cost is far smaller than "add iOS support" usually implies
   (see Decision 4): the imported code has no Apple-specific paths and
   no executable-memory / `mmap` path on Apple, so iOS is a packaging
   concern, not a porting one.

The hard part of this change is **not** `Package.swift` — it is a
structural fact about SwiftPM that the original proposal phrased as a
one-liner. A SwiftPM target is rooted at a single directory, and
`publicHeadersPath` is resolved *relative to that directory*. gaby-vm's
public headers (`include/gaby_vm/`) and sources (`src/`) are siblings at
the repository root, so no single target can name both without either
rooting the target at the repository root (`path: "."`, with a fragile
exclude list) or moving the two directories under one parent. We choose
to move. Everything else in this design follows from that choice and
from reproducing, under SPM, the four things the CMake target does that
"point SPM at the same files" silently drops.

## Goals / Non-Goals

**Goals:**

- A committed top-level `Package.swift` that builds a single static C++
  library product `gaby_vm` from the same sources as the CMake build, on
  macOS and iOS.
- The SPM build reproduces the CMake target's private compile
  environment exactly: the VIXL gating defines and the private `src/`
  header search path. Without these the imported VIXL code does not
  compile correctly.
- One canonical on-disk layout that both build systems point at — no
  duplicated or symlinked source trees.
- `version()` has a single source of truth that both builds consume, with
  no build-time code generation.
- CMake stays the canonical, strict-warning build; it must still build
  green after the relocation.
- The library **compiles and links for iOS** (Simulator, and the device
  SDK), verified in CI. The embedder is an iOS-supporting library, so
  iOS build compatibility is a first-class, checked guarantee of this
  change — not an aspiration.
- A verification step that proves the SPM build *closes and is usable*
  on macOS, runnable in CI.

**Non-Goals:**

- Any change to C++ leaf semantics or the public API surface. Files
  move; their contents (other than `version.h` and the deleted
  `version.cc.in`) do not.
- A Swift `import gaby_vm` story. The embedder is a C++ SPM target; it
  consumes gaby-vm through `#include "gaby_vm/…"`, which needs no module
  map. Swift/C++ interop is out of scope (see Decision 6).
- An `.xcframework` / prebuilt-binary distribution. The package is a
  source target so the consumer's toolchain compiles it — required for
  the iOS-embedding work sequenced later.
- An iOS app, UI demo, or `demos/ios/` Xcode project. iOS is in scope as
  a *build-compatibility* guarantee (Decision 7), verified by building
  the library for the iOS SDK — not by a runnable app. No Swift / ObjC ↔
  C++ bridge or interop is introduced.
- Per-file warning policy under SPM. CMake remains the strict gate
  (Decision 5).
- Linux as a tested SPM target. SPM-on-Linux C++ works in principle but
  is best-effort, not a project test target.

## Decisions

### 1. Single target, wrapped under `Sources/gaby_vm/`

A SwiftPM target has one `path`, and `publicHeadersPath` is relative to
it. `path: "src", publicHeadersPath: "include"` resolves to
`src/include/` (does not exist); `publicHeadersPath: "../include"` is
rejected (must stay within the target). So a single target spanning the
sibling `src/` and `include/` directories is impossible without rooting
at the repository (`path: "."`).

**Chosen:** relocate both directories under one SPM-conventional parent,
preserving the internal `src/` tree unchanged:

```
Sources/gaby_vm/                 ← SPM target path
├── include/
│   └── gaby_vm/*.h              ← 6 public headers (publicHeadersPath: "include")
└── src/                         ← identical internal tree, re-rooted
    ├── *.cc, *-vixl.h
    ├── aarch64/*.cc, *.h
    ├── gaby_vm/*.cc
    └── CMakeLists.txt
```

```swift
.target(
    name: "gaby_vm",
    path: "Sources/gaby_vm",
    publicHeadersPath: "include",           // → Sources/gaby_vm/include; preserves #include "gaby_vm/…"
    cxxSettings: [ .headerSearchPath("src"), /* defines — Decision 2 */ ]
)
```

The `src/` subdir is kept (rather than flattened into the target root) so
the VIXL import boundary stays legible: the extraction map's
`src/aarch64/…` references re-root to `Sources/gaby_vm/src/aarch64/…`
with their structure intact, and `.headerSearchPath("src")` is the direct
analogue of the CMake target's `PRIVATE src/`.

**Rejected:**

- **`path: "."` + exclude list** (no file moves). Works, but the target
  becomes "the whole repository," and the `exclude` list must enumerate
  every non-source top-level directory (`test`, `bench`, `demos`, `docs`,
  `openspec`, `cmake`, `build`, `third_party`, …). Every new top-level
  directory then silently risks an "unhandled files" warning until
  someone adds it. Fragile by construction.
- **Flatten into `Sources/gaby_vm/` (no `src/` level)**. Marginally more
  "SPM-native," but erases the `src/` concept that the docs and the VIXL
  extraction map anchor on, for no build-level benefit.
- **Symlink bridge** (`Sources/gaby_vm/` of symlinks into `src/` +
  `include/`). Keeps CMake untouched but ships two views of the same
  files; git symlinks are fragile across checkouts and can confuse
  SwiftPM file discovery. Rejected as a hack dressed as "no move."

### 2. Reproduce the CMake private compile environment in `cxxSettings`

The CMake target (`src/CMakeLists.txt`) sets three `PRIVATE` defines and
a `PRIVATE src/` include directory. The imported VIXL code is `#ifdef`-gated
on those defines; without them it compiles to the wrong thing or not at
all. These are **not optional** under SPM:

```swift
cxxSettings: [
    .headerSearchPath("src"),                            // CMake PRIVATE src/
    .define("VIXL_INCLUDE_TARGET_A64"),
    .define("VIXL_INCLUDE_SIMULATOR_AARCH64"),
    .define("VIXL_DEBUG", .when(configuration: .debug)), // CMake's $<$<CONFIG:Debug>:VIXL_DEBUG>
]
```

The defines are scoped to the target (SPM has no INTERFACE-define
leakage), matching the CMake `PRIVATE` scoping — consumers never see
`VIXL_*`. The out-of-scope VIXL defines (`VIXL_INCLUDE_TARGET_A32*`,
`VIXL_CODE_BUFFER_*`, `VIXL_HAS_SIMULATED_MMAP`) are deliberately never
set, exactly as in CMake.

### 3. `version()` header-only; CMake drops its project version

Today `version()` is defined in `version.cc`, generated by CMake's
`configure_file(src/version.cc.in → version.cc)` substituting
`@PROJECT_VERSION@`. SPM has no configure step, the `.in` extension is
not a source SwiftPM recognizes, and a committed `.cc` reintroduces a
second place the version string lives. The decision is to make the header
the sole source of truth:

```cpp
// include/gaby_vm/version.h
#define GABY_VM_VERSION_STRING "0.0.1"          // single source of truth
namespace gaby_vm {
inline const char* version() { return GABY_VM_VERSION_STRING; }
}
```

Consequences:

- Delete `src/version.cc.in` and the `configure_file` +
  `target_sources(... version.cc)` lines in the CMakeLists.
- Drop `VERSION` from the root `project(gaby_vm …)` clause. Verified that
  `@PROJECT_VERSION@` in `version.cc.in` was the **only** consumer of
  `PROJECT_VERSION`; nothing else references it, so removing it is inert.
- An `inline` definition may not emit a linkable external symbol in the
  `.a` (it inlines / emits linkonce). That is correct for a C++ embedder
  that includes the header. Noted as forward-incompat only for a
  hypothetical C-ABI `dlsym("gaby_vm_version")`, which no consumer needs.

**Rejected:** a committed static `version.cc` (reintroduces drift vs the
header); a `.define("GABY_VM_VERSION_STRING", …)` set in both build
systems (two sources of truth, and quoting a string through SPM `.define`
is finicky); CMake parsing the header to keep `project(VERSION)`
(considered, but the chosen "drop it" answer is simpler and the CMake
version was only cosmetic for a static lib).

### 4. Platforms follow the embedder

The imported simulator's only host-syscall path is `Simulator::Mmap` /
`Munmap`, gated by `VIXL_HAS_SIMULATED_MMAP`, which is itself defined only
under `#if __linux__` (`src/aarch64/simulator-aarch64.h:3310`). On Apple
platforms that code does not compile. The broad scan for `__APPLE__` /
`TARGET_OS` conditionals in the imported tree found none — the only
platform fork is that `__linux__` block. Combined with the project's
no-JIT / no-RWX charter, the imported simulator is plain portable C++ on
iOS and macOS.

So the package declares iOS and macOS support but imposes **no
restrictive minimum** beyond the toolchain default; the embedder app's
deployment target is the effective floor. One caveat is recorded for
later: individual C++20 libc++ features (the public headers use
`std::span`, `std::variant`, `std::function`) carry their own OS
availability annotations. If one bites on an old OS, that is the moment
to raise the declared minimum — not now.

iOS is not merely *declared* but *verified*: Decision 7 builds the
library for the iOS SDK, so "iOS support" is a checked compile + link
guarantee. What is out of scope is a runnable iOS app, not iOS itself.

### 5. CMake is the strict warning gate; SPM uses defaults

The CMake build applies a strict project warning policy and a relaxed
per-file policy to imported VIXL sources. SwiftPM `cxxSettings` are
per-target only — a single target cannot carry both. Per the project
owner's call, warnings are not gated under SPM: the SPM build uses
reasonable compiler defaults and does **not** pass `-Werror`. CMake
remains the build where warnings are enforced. This keeps the SPM target
single (Decision 1) and avoids splitting into a `gaby_vm_vixl` +
`gaby_vm` two-target shape purely to host two warning policies.

### 6. Static library product, no module map

```swift
products: [ .library(name: "gaby_vm", type: .static, targets: ["gaby_vm"]) ]
```

`.static` matches the CMake `STATIC` library and the embedder's
expectation. No `module.modulemap` is shipped: a C++ SPM dependent
reaches the headers through `publicHeadersPath` and `#include
"gaby_vm/…"` with no module map needed. A module map only matters for
Swift `import gaby_vm`, which is out of scope — and would buy little
anyway, since the public headers' `std::span` / `std::variant` /
`std::function` / templated / Pimpl surface does not import cleanly under
Swift/C++ interop. The original proposal's `include/gaby_vm/module.modulemap`
was also mis-located (a custom module map must sit at the root of
`publicHeadersPath`, i.e. `include/module.modulemap`); dropping it
removes the question entirely.

Precise note: SwiftPM still auto-synthesizes an umbrella module map for
the C-family target — that cannot be fully opted out of. "No module map"
means we ship no *custom* one and do not depend on Swift `import`. A C++
`#include` consumer (the real embedder, and the macOS demo) needs only
the header search path, which SwiftPM always provides, and never touches
the synthesized module.

### 7. Verification: a macOS SPM demo plus an iOS library build

The original proposal verified by diffing the public symbols of the SPM
`.a` against the CMake `.a`. That is brittle — different defines, the new
header-only `version()`, and warning-driven inlining make the two symbol
sets legitimately diverge, producing false mismatches. It also conflated
"link a Swift executable" (interop) with "link a C++ executable" (trivial
on macOS). It is dropped in favour of two checks that match the two
things this change promises — *usable on macOS* and *buildable for iOS*.

**macOS — an SPM executable demo.** A small C++ `.executableTarget` in
`Package.swift` (sources e.g. under `demos/`) consumes the `gaby_vm`
product, calls `version()`, and runs a tiny guest program (the same
`host_add`-style snippet as the CMake `demos/cli`). `swift run`
compiling, linking, and executing it proves the SPM build closes and
produces a *usable* library on macOS — exactly the path the real C++
embedder takes — and doubles as the package's macOS demo. It is a
dev/demo target, not part of the library product, so package consumers
never build it.

**iOS — a library build for compatibility.** The embedder is an
iOS-supporting library, so gaby-vm must compile and link for iOS. CI
builds the `gaby_vm` *library* for the iOS SDK via `xcodebuild`
(`-scheme gaby_vm -destination 'generic/platform=iOS Simulator'`, and
optionally the device SDK). This is a **build-only** check — no app
bundle, no code signing, no run, no Swift/ObjC bridge. It is the iOS half
of "iOS is in scope": the guarantee is that the library is iOS-buildable,
verified — not that a demo app runs. (`xcodebuild` understands a root
`Package.swift` directly and auto-generates a scheme per product.)

Both checks run on a macOS CI host and are skipped where the toolchain is
absent (e.g. `swift` / Xcode on a Linux runner). CMake remains the
platform-independent gate.

### 8. CMake stays canonical; the relocation is the bulk of the work

`Package.swift` is ~30 lines. The real work is the path-reference ripple
from moving `src/` and `include/` under `Sources/gaby_vm/`. The gating
check for the change is **CMake still builds green** after the move,
since CMake remains the canonical strict build. Known reference sites to
update (non-exhaustive; an implementation sweep should grep for the old
paths):

```
CMakeLists.txt (root)             re-root add_subdirectory(src) → Sources/gaby_vm/src; drop project VERSION
Sources/gaby_vm/src/CMakeLists.txt   delete version.cc.in lines; update PUBLIC/PRIVATE include dirs
docs/refs/vixl-extraction-map.md     src/… → Sources/gaby_vm/src/…
docs/architecture.md                 import-boundary / marker-convention path references
docs/build.md, docs/conventions.md   target / include-structure references
.clangd, per-dir .clang-format        move with files; check for path references
test/ bench/ demos/ CMakeLists        confirm they consume the target's propagated include dir, not hardcoded ../include / ../src
.gitignore                            add .build/, .swiftpm/, Package.resolved
README.md                            new "### SwiftPM" subsection under "### Embedding"
```

## Rejected alternatives (summary)

| Area | Rejected | Why |
|---|---|---|
| Layout | `path: "."` + exclude list | Target = whole repo; exclude list rots with every new top-level dir |
| Layout | Flatten `Sources/gaby_vm/*` | Erases `src/` anchor used by docs + extraction map; no build benefit |
| Layout | Symlink bridge | Two views of one tree; git-symlink + SwiftPM-discovery fragility |
| version | Committed static `version.cc` | Reintroduces drift vs the header |
| version | `.define` in both builds | Two sources of truth; `.define` string quoting is finicky |
| version | CMake parses header for `project(VERSION)` | Simpler to drop the (cosmetic) CMake version outright |
| Warnings | Two-target split for per-file policy | Single target is simpler; CMake already gates warnings |
| Module map | Ship `module.modulemap` | Not needed by a C++ consumer; Swift import out of scope; was mis-located |
| Verify | `nm` symbol-parity diff | Brittle (defines/inlining/version differences); C++ smoke link is stronger |

## Risks / Open items

- **Relocation ripple.** The move touches CMake, docs, the VIXL
  extraction map, and per-dir tooling configs. Risk is a missed path
  reference; mitigated by the "CMake builds green" gate and a grep sweep
  for `"/src/"` and `"/include/"` references during implementation.
- **C++20 libc++ availability on old Apple OSes.** Deferred by Decision
  4 (follow the embedder). If a consumer targets an OS where a used
  C++20 library feature is unavailable, the declared minimum gets raised
  then.
- **swift-tools-version floor.** `.cxx20` and C++ targets need a modern
  toolchain; `swift-tools-version:5.9` is a safe conservative floor (the
  development host is 6.3). To confirm at implementation time against the
  embedder's toolchain.
- **iOS is verified by library build, not by an app.** This change
  verifies gaby-vm *compiles and links for iOS* (Decision 7); it ships no
  runnable iOS app. A `demos/ios/` Xcode example and any on-device run
  remain follow-up work. Risk: the `xcodebuild` iOS build needs Xcode on
  the CI host — on a Linux runner the iOS check is skipped, so iOS
  buildability is only enforced where Xcode is available.
