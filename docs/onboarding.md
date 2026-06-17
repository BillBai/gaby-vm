# Onboarding Guide

[Chinese version](onboarding.zh-cn.md)

This guide gives a first path through the repository. It starts with the project
shape, then points you to the build, test, benchmark, and design documents that
matter next. It does not repeat every detail from `architecture.md`, `build.md`,
`testing.md`, or `bench/README.md`; it tells you which document and which code
area to read when you need the details.

> This is an orientation guide, not the authority. If it conflicts with code or
> the focused design documents, follow the code and those documents. A few known
> documentation drifts are listed at the end.

---

## 0. What This Project Does

gaby-vm is an embeddable AArch64 instruction interpreter. Its instruction
semantics come from the VIXL AArch64 simulator. The main optimization strategy
is:

```text
predecode once -> cache decoded dispatch target -> execute cached path repeatedly
```

The project boundaries matter:

- It is not a JIT. Runtime code generation, executable memory allocation, and
  RWX memory are out of scope. Predecoded entries are ordinary data, which keeps
  the project suitable for iOS embedding.
- It is not a new IR. The cache stores which VIXL leaf function should execute
  a given instruction. It does not translate instructions into a separate
  intermediate representation.
- It is not a full system emulator. V1 targets EL0/user-mode A64 execution. It
  has no MMU, device model, or complete exception-level model.

iOS is the primary no-JIT target. macOS and POSIX-like environments such as
Linux, Android, and HarmonyOS should also work. Windows is not a target for now.

The project rule is simple: correctness first, speed second.

---

## 1. Architecture

### 1.1 Two Modes, One Set of Semantics

gaby-vm has two execution modes. Both execute the same instruction semantics;
they differ only in how they find the next VIXL leaf function.

| Mode | Public entry points | Internal path | Use |
|------|---------------------|---------------|-----|
| **cache mode** | `Simulator::RunFrom` / `StepOnce` | Predecode caches each instruction's dispatch target. The steady-state loop fetches the cache entry and calls the leaf. | Production path and performance target. |
| **decoder mode** | `Simulator::DebugRunFrom` / `DebugStepOnce` | Keeps VIXL's original `Decoder -> VisitNamedInstruction -> leaf` flow and decodes each instruction at execution time. | Historical path, diagnostics, and differential test baseline. |

The important invariant is that both modes call the same leaf functions. Both
look up a shared `FormToVisitorFnMap` through the same `form_hash`, obtain the
same `Simulator` member-function pointer, then call `(this->*pmf)(pc)`.

- cache mode fetches entries in `ExecuteInstructionCached` in
  `Sources/gaby_vm/src/aarch64/simulator-aarch64.h`, inside the `// gaby-vm`
  marker region.
- decoder mode runs through `ExecuteInstruction` in the same header. The leaf
  lookup lives in `simulator-aarch64.cc` as `Hash(form) -> map.find ->
  (this->*fn)(instr)`.

This shared-leaf design is what makes the project maintainable:

- Speed work changes the repeated execution path, not the semantics.
- Tests can compare the two modes bit-for-bit. Given the same bytes, cache mode
  and decoder mode must produce the same registers and memory writes. See
  `vixl_port` and `ShadowRunner` in the testing section.

> The two loops do not switch modes mid-run. A `RunFrom` call stays in cache
> mode. A `DebugRunFrom` call stays in decoder mode. This avoids corrupting
> execution state such as `form_hash_`, `last_instr_`, and MOVPRFX chaining.

### 1.2 What PredecodeCache Stores

`PredecodeCache` is the core data structure for cache mode.

- Public interface: `Sources/gaby_vm/include/gaby_vm/predecode_cache.h`
- Implementation: `Sources/gaby_vm/src/gaby_vm/predecode_cache.cc`

The main API is:

```cpp
gaby_vm::PredecodeCache cache;
auto status = cache.RegisterCodeRange(code_ptr, size_bytes);
// Run cache mode only when status == RegistrationStatus::Ok.
```

`RegisterCodeRange` decodes the whole code range once at registration time. Each
instruction produces one fixed-size `PredecodedEntry`, currently 16 bytes, so
runtime lookup can use flat indexing by `(pc - start) / 4`.

Each entry stores:

- `form_hash`: the instruction form hash used for the shared leaf lookup.
- `flags`: bit flags, for example a BTI-related flag that lets most
  instructions skip the runtime BTI check.
- `leaf`: an opaque pointer to the `Simulator` member function that implements
  the instruction.

Predecode cost belongs to registration, not to the steady-state execution loop.
At runtime, the simulator finds the containing code range, first through the
cached current range and then through `cache.FindRange` if needed. It indexes
the entry by PC offset and calls the cached leaf.

### 1.3 Code Map

This is a standalone project, not an in-place VIXL fork. The reference VIXL tree
lives outside the repo at `../vixl` and is used for study and comparison. This
repository imports only a controlled subset.

gaby-vm-owned code carries "the gaby-vm authors" copyright. The owned
implementation is small:

```text
Sources/gaby_vm/include/gaby_vm/   public API, namespace gaby_vm
  gaby_vm.h            facade, mostly forwards version information
  simulator.h          Simulator: two execution modes, typed register I/O, hooks
  predecode_cache.h    PredecodeCache plus PredecodedEntry, CodeRange, RegistrationStatus
  registers.h          typed register identifiers plus RegisterFile snapshots
  shadow_runner.h      ShadowRunner differential oracle for tests
  version.h            header-only version data

Sources/gaby_vm/src/gaby_vm/       owned implementation
  simulator.cc         Pimpl wrapper around VIXL Simulator, public API plumbing
  predecode_cache.cc   RegisterCodeRange, FindRange, predecode visitor
  shadow_runner.cc     lockstep two-mode runner, register and memory diffing
```

Everything else under the imported tree comes from VIXL, keeps VIXL's BSD-3
Clause license, and mirrors upstream layout:

```text
Sources/gaby_vm/src/               shared VIXL root: utils, cpu-features, intrinsics
Sources/gaby_vm/src/aarch64/       AArch64 simulator, decoder, instructions,
                                   logic, operands, registers, disasm, debugger
```

The import boundary follows the tiers in `docs/refs/vixl-extraction-map.md`.
The shipping library includes only Tiers 1 through 3. Tier 0 files such as the
assembler, macro-assembler, and code-buffer do not ship in the library because
gaby-vm consumes instruction bytes and does not generate code. The test-only
assembler island is the one exception, covered below.

Any edit to an imported file must use marker comments so drift from upstream can
be audited:

- Single-line edits: add `// gaby-vm:` above the changed line, then explain the
  reason in the next `//` comment.
- Multi-line edits: wrap the block with `// gaby-vm BEGIN:` and
  `// gaby-vm END`.

List all marker sites with:

```sh
git grep -nE 'gaby-vm( BEGIN| END|:)' Sources/gaby_vm/src/
```

Most cache-mode changes, including `ExecuteInstructionCached`, `StepOnce`,
branch hooks, and re-entrant cursor handling, live in marker regions in
`simulator-aarch64.h`.

### 1.4 Memory and Threading Model

The memory model uses the host address space directly. A guest pointer is a host
pointer. Guest loads and stores are ordinary host memory accesses into the
embedder's heap, stack, or globals. There is no MMU, address translation,
bounds check, or guest/host isolation layer. If guest code writes out of bounds,
the host process may crash with `SIGSEGV` or corrupt host state.

That means:

- The embedder must place guest code and data at the host addresses the guest
  expects.
- The embedder owns the lifetime, alignment, and aliasing rules for all memory
  the guest may touch.
- An untrusted or malformed guest is a host process bug by construction.

The benefit is that the interpreter loop avoids bounds checks, which matters for
cache-mode speed and fits the iOS embedding model where the embedder already
shares one address space.

The threading model is one `Simulator` instance per physical host thread,
usually initialized lazily. A simulator instance is not designed for sharing
across threads. Its registers, system registers, and internal scratch state
belong to the thread that owns the instance. gaby-vm does not install this for
you; embedders should store the per-thread instance in `thread_local`, TLS, or
their own thread context object.

Read `docs/architecture.md` for the authoritative version.

### 1.5 Minimal Embedding Skeleton

`demos/cli/main.cc` is the official end-to-end embedding example. It builds a
small guest function by hand, registers it in the cache, runs it through
`RunFrom`, and uses a branch hook to forward a guest call to a host function
using the C ABI.

The shape is:

```cpp
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/simulator.h"
#include "gaby_vm/registers.h"

gaby_vm::PredecodeCache cache;
if (cache.RegisterCodeRange(code, size_bytes) !=
    gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
  // Handle registration failure.
}

std::vector<uint8_t> stack(gaby_vm::Simulator::kMinStackSize);
gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

// Optional: intercept branches to call host functions or implement FFI.
sim.SetBranchHook(my_hook, &my_ctx);

// Optional: pass arguments according to the AArch64 C ABI.
sim.Write(gaby_vm::GpRegister::X0, arg0);

sim.RunFrom(reinterpret_cast<uintptr_t>(code));
uint64_t result = sim.Read(gaby_vm::GpRegister::X0);
```

Use `DebugRunFrom` instead of `RunFrom` to execute through decoder mode. Decoder
mode does not require a non-empty cache and is useful for tracing, debugging,
and comparisons.

Run the demo with:

```sh
./build/debug/demos/cli/gaby-vm
./build/debug/demos/cli/gaby-vm --version
```

---

## 2. Build

### 2.1 Toolchain Requirements

- C++20. Public headers use `std::span` and `std::variant`.
- CMake 3.21 or newer. The presets file uses the 3.25 schema.
- Ninja.
- GCC, Clang, or AppleClang. MSVC is not supported, and Windows is not in
  scope.

### 2.2 Fast Path With Presets

The repository provides `dev-debug` and `dev-release` presets. Both use Ninja
and enable `compile_commands.json`, tests, and demos.

```sh
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

Debug artifacts go under `build/debug/`. Release artifacts go under
`build/release/`. Release tests currently run with `ctest --test-dir
build/release`; there is no release test preset yet.

### 2.3 Build Options

Top-level `CMakeLists.txt` exposes these options:

| Option | Default | Purpose |
|--------|---------|---------|
| `GABY_VM_BUILD_TESTS` | ON for top-level builds | Builds `test/`. |
| `GABY_VM_BUILD_DEMOS` | ON for top-level builds | Builds `demos/cli`. |
| `GABY_VM_BUILD_BENCHMARKS` | ON for top-level builds | Builds the `bench/` throughput harness. |
| `GABY_VM_BUILD_NATIVE_BASELINE` | OFF | Builds native comparison tools. Requires benchmarks and an arm64 host. |

The first three default to `${PROJECT_IS_TOP_LEVEL}`. They are ON when you build
this repository directly and OFF when another CMake project consumes it through
`add_subdirectory`.

`GABY_VM_BUILD_NATIVE_BASELINE` is a developer-only option. It places guest
AArch64 bytes into executable memory and runs them directly on the host CPU, so
the host must be arm64. It provides the denominator for "how far from native"
benchmark numbers.

### 2.4 Two CMake Details Worth Knowing

You usually do not need to touch CMake, but two details prevent confusion:

- Warning policy is split. gaby-vm-owned code uses strict flags through
  `gaby_vm_apply_compile_flags`. Imported VIXL sources use relaxed per-file
  flags through `gaby_vm_apply_imported_compile_flags` because upstream code
  triggers pedantic warnings. Both source classes live in the same `gaby_vm`
  target.
- VIXL compile definitions are `PRIVATE`: `VIXL_INCLUDE_TARGET_A64`,
  `VIXL_INCLUDE_SIMULATOR_AARCH64`, and `VIXL_DEBUG` in Debug builds. Consumers
  do not inherit them through `INTERFACE`. Tests and benchmarks that include
  `vixl::aarch64::*` headers must use the privileged build pattern from
  `docs/build.md`: private include access to `src/` plus the same VIXL macros.

### 2.5 Consuming the Library

CMake consumers can use:

```cmake
add_subdirectory(third_party/gaby-vm)
target_link_libraries(your_target PRIVATE gaby_vm::gaby_vm)
```

The repository also has a top-level `Package.swift` for C++ SwiftPM consumers.
Consumers must set C++20 themselves, for example with
`cxxLanguageStandard: .cxx20`, because the language standard does not propagate
across Swift packages. CMake remains the authoritative build for tests and
benchmarks. Both builds compile the same source tree.

---

## 3. Test

### 3.1 Test Layout

Tests live under `test/` as small standalone executables. Each test links
`gaby_vm::gaby_vm` and is registered with CTest through `add_test`. The project
currently uses handwritten tests rather than GoogleTest or Catch2. That is a
practical choice, not a permanent rule.

Run tests with:

```sh
ctest --preset dev-debug
ctest --test-dir build/debug -R <regex>
ctest --test-dir build/debug -N
```

### 3.2 Registered Test Groups

Use `test/CMakeLists.txt` as the source of truth. At a high level:

- Smoke and build-contract tests: `smoke`, `simulator_smoke`, and
  `instructions_aarch64_constexpr_smoke`.
- Correctness tests: `simulator_correctness`, which runs hand-encoded sequences
  through both cache mode and decoder mode and checks the expected final state.
- Public API behavior tests: typed register I/O, constructor stack validation,
  branch hooks, and re-entrancy.
- Differential oracle tests: `shadow_runner` and `workload_shadow`.
- Ported VIXL suites: `vixl_port_integer`, `vixl_port_fp`, and
  `vixl_port_neon`. The same assembler island also has infrastructure tests
  such as `vixl_asm_island_smoke`, `vixl_asm_harness_smoke`, and harness
  self-tests for cache, decoder, reference, and baseline paths.

The guard command `-R vixl_port` matches the three core ported suites.

### 3.3 vixl_port: The Hot-Path Guard Rail

`vixl_port` is the broadest correctness guard. It exists to catch regressions
introduced by predecode and dispatch optimizations. It live-assembles upstream
VIXL `TEST()` bodies, then runs each body through both gaby-vm modes.

The suite works like this:

- `test/test_support/vixl_asm/` contains a test-only Tier 0 assembler island:
  VIXL assembler, macro-assembler, code-buffer, and test infrastructure copied
  at the import SHA recorded in `vixl-extraction-map.md`. It builds as
  `gaby_vm_vixl_asm_testonly`, private-links `gaby_vm::gaby_vm`, has no `::`
  alias, is gated by `GABY_VM_BUILD_TESTS`, and uses
  `VIXL_CODE_BUFFER_MALLOC` only. The assembled bytes are ordinary `malloc`
  data passed to the gaby decoder. They are never executed by the host CPU.
- Because assembly happens at test runtime, tests that bake real process
  addresses into instructions exercise real in-process memory. Load/store,
  ADR, literal, atomic, exclusive, and CAS-style bodies run against actual
  memory.
- Each body uses two oracles:
  - Differential oracle: cache mode and decoder mode must match.
  - Absolute oracle: gaby-vm must match a VIXL reference simulator.

Run this before and after changing shared execution hot paths: decode/dispatch,
the predecode cache, or imported VIXL leaf semantics.

```sh
ctest --test-dir build/debug -R vixl_port
```

The suite is self-contained and does not need `../vixl`.

Notes:

- It cannot build under ASan. Its memory oracle copies bytes from a live C++
  stack frame, which conflicts with ASan stack red zones. The safety check here
  is the two-mode plus reference differential oracle, not sanitizer coverage.
- It has baseline count guards. Each family pins how many tests run and how many
  skip. If coverage moves, the suite fails so skipped coverage cannot shrink
  silently. Intentional changes use `VIXL_PORT_REBASELINE=1` and update the
  baseline in the same commit.
- Some cases intentionally skip unsupported, nondeterministic, or
  data-in-stream patterns. `VIXL_PORT_TRACE=1` prints skip reasons.
- After a VIXL upgrade, there are no fixtures to regenerate. Re-copy the
  assembler island at the new SHA, update `vixl-extraction-map.md`, and rerun.

### 3.4 Adding a Test

- Add the executable and CTest registration in `test/CMakeLists.txt`.
- Use `gaby_vm_apply_compile_flags`.
- Tests that touch only the public API should not use the privileged VIXL build
  pattern.
- Tests that need `vixl::aarch64::*` headers should follow the privileged build
  pattern from `docs/build.md`.
- Return nonzero on failure. Print the failing subcase and actual versus
  expected values.

Encoding policy: the shipping tree has no assembler. Tests such as
`simulator_correctness` use raw `uint32_t` instruction words. When you need a
new encoding, generate the hex with an external tool such as `llvm-mc` during
authoring, then copy the word into source. Build and runtime must not depend on
that assembler.

---

## 4. Benchmark

### 4.1 CTest Checks Correctness, bench Measures Speed

`bench/` contains developer-invoked throughput harnesses. Benchmarks are not
registered with CTest. Run the binaries from the build directory. They measure
how fast a fixed instruction workload executes under a selected mode.

Read `bench/README.md` for the authoritative harness description.

### 4.2 Benchmark Binaries

| Binary | Workload | Use |
|--------|----------|-----|
| `bench_baseline` | Upstream-VIXL generated mixed workload, about 64k dynamic instructions per iteration and 68 percent NEON. | Synthetic stress test, not representative business logic. |
| `bench_smoke` | 32 straight-line ALU instructions assembled with `llvm-mc`. | Millisecond-scale harness smoke test. |
| `bench_business` | Five compiled C microkernels. | Representative "how slow is iOS business logic" benchmark. |
| `native_baseline`, `native_smoke`, `native_business` | Same bytes run directly on an arm64 host CPU. | Native denominator. Requires `GABY_VM_BUILD_NATIVE_BASELINE=ON`. |

`bench_business` kernels:

- `parse`: variable-length record parsing.
- `hash`: FNV-1a, integer dependency chain, few branches.
- `struct`: structure-array transforms.
- `fsm`: byte-wise state-machine scanner with unpredictable dispatch.
- `applogic`: the only FP/NEON workload, with scalar double operations similar
  to CGFloat geometry.

### 4.3 Build and Run

Use Release builds for meaningful performance numbers:

```sh
cmake --preset dev-release
cmake --build --preset dev-release
```

Enable the native denominator on arm64 hosts:

```sh
cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON -DGABY_VM_BUILD_NATIVE_BASELINE=ON
cmake --build --preset dev-release
```

Example runs:

```sh
./build/release/bench/bench_business --mode cache   --seconds 2.0
./build/release/bench/bench_business --mode decoder --seconds 1.0
./build/release/bench/bench_business --kernel hash  --mode cache
./build/release/bench/native_business               --seconds 1.0
./build/release/bench/bench_business --verify
./build/release/bench/bench_baseline --help
```

The primary output key is `iterations_per_second`, which is comparable across
native, decoder, and cache runs.

### 4.4 Which Numbers Matter

- For the representative slowdown number, compare cache-mode `bench_business`
  against `native_business` per kernel. The current baseline is roughly
  6.5 ns/instruction for the scalar kernels, with slowdown ranging from about
  19x for `hash` to about 210x for `struct`, depending mostly on native-side
  IPC. `applogic`, the FP/NEON workload, is slower at about 10 ns/instruction
  and about 330x versus native.
- After changing a leaf or benchmark kernel, run `bench_business --verify`
  first. It cross-checks cache mode against decoder mode and validates `x0`
  against the committed expected value.
- When a change targets execution speed, quote before/after numbers from this
  harness. Do not estimate by inspection.

### 4.5 Measurement Expectations

The V1 harness is meant to answer order-of-magnitude questions such as whether
cache mode helped by 3x. It is not a publication-grade performance lab. On a
normal development machine, run-to-run variance can be large. Run more than
once, look at the scale of the result, and avoid over-reading the last digits.
Use an idle machine on power when possible.

---

## 5. Where to Read Next

| Topic | Read |
|-------|------|
| Architecture, memory/threading model, VIXL import boundary, marker convention | `docs/architecture.md` |
| Internal build structure, targets, warning policy, VIXL macro scope | `docs/build.md` |
| Coding conventions, namespaces, license headers, markers | `docs/conventions.md` |
| Test strategy, `vixl_port`, encoding policy | `docs/testing.md` |
| Performance method and business microkernels | `bench/README.md` |
| VIXL import tier list | `docs/refs/vixl-extraction-map.md` |
| Normative capability requirements | `openspec/specs/` |
| User-facing build and embedding instructions | `README.md` |
| Project goal and agent rules | `AGENTS.md`, also reached through `CLAUDE.md` |
| Design and research notes by date | `docs/refs/` |

Fast paths for reading code:

- gaby-vm changes on top of VIXL:
  `git grep -nE 'gaby-vm( BEGIN| END|:)' Sources/gaby_vm/src/`
- cache-mode hot path:
  `Sources/gaby_vm/src/aarch64/simulator-aarch64.h`, `ExecuteInstructionCached`
- public API:
  `Sources/gaby_vm/include/gaby_vm/`
- complete embedding example:
  `demos/cli/main.cc`

---

## 6. Known Documentation Drift

These documents currently lag the code. Treat the code as authoritative:

- C++ standard: top-level `CMakeLists.txt` sets C++20 and the library target
  requires `cxx_std_20`. Some older docs still mention C++17. The real
  requirement is C++20.
- Benchmark binary list: `docs/build.md` mentions `bench_baseline` and
  `bench_smoke`, but not `bench_business` or the native binaries.
- Test target list: `docs/testing.md` lists only part of what
  `test/CMakeLists.txt` registers. See section 3.2 above for the current shape.
