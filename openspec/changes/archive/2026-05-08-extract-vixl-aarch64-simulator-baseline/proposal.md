## Why

Gaby-VM's optimization plan (predecode once → cache decoded dispatch target → execute cached path repeatedly) only has meaning relative to a working VIXL-style simulator running inside this repository. Today the repo is a build skeleton with a smoke test and a `version.cc` stub — there is no simulator code, no proof the imported subsystems link cleanly, and nothing the rest of the project can build on. Without an imported simulator, downstream changes (the decode-cache, fallback paths, embedding API) have nothing to extend. This change imports the VIXL AArch64 simulator and the dependencies it needs to build and run, wires it into the existing CMake build with **no edits to imported files**, and proves the import works by constructing a `Simulator` and successfully executing a single instruction.

## What Changes

- Import the VIXL files needed for an AArch64 simulator that builds and runs — drawing from **Tiers 1, 2, and 3** of `docs/refs/vixl-extraction-map.md`. Concretely:
  - **Tier 1** (must-have): shared root files (`globals-vixl.h`, `utils-vixl`, `compiler-intrinsics-vixl`, `cpu-features`, `platform-vixl.h`) plus `src/aarch64/` simulator core (`simulator-aarch64`, `decoder-aarch64`, `decoder-constants-aarch64`, `decoder-visitor-map-aarch64`, `instructions-aarch64`, `constants-aarch64`, `cpu-features-auditor-aarch64`, `registers-aarch64`, `operands-aarch64`, `abi-aarch64`, `cpu-aarch64`, `logic-aarch64`, `simulator-constants-aarch64`, `pointer-auth-aarch64.cc`).
  - **Tier 2**: `disasm-aarch64.h` / `.cc` — `Simulator`'s constructor unconditionally creates a `PrintDisassembler`, and `simulator-aarch64.h` includes the header. Stubbing them would require structurally editing `simulator-aarch64.h`/`.cc`, which this change does not do.
  - **Tier 3**: `debugger-aarch64.h` / `.cc` — `Simulator` constructs a disabled `Debugger`. Stubbing it out would require the same structural edit, so it is imported instead.
- **Tier 0 stays out**: the assembler / macro-assembler / SVE-assembler / code-buffer / pool-manager / `assembler-base-vixl.h` / `code-generation-scopes-vixl.h` / `invalset-vixl.h` / `macro-assembler-interface.h`, all of `src/aarch32/`, and VIXL build tooling (`SConstruct`, `tools/`).
- The actual file list is **driven by the build** — anything an imported header `#include`s and the linker requires gets pulled in until the simulator subset compiles and links cleanly. The tier split is the scoping rule, not a fixed file manifest.
- **Edits to imported files are permitted, but must stay minimal and non-structural**, and every changed location must be bracketed by a Gaby-VM marker comment so the delta against upstream is auditable and re-syncable. The exact comment syntax is settled in `design.md`.
  - **Allowed** (must still be commented): `#include` path adjustments, namespace nudges, small portability fixes, build-flag plumbing, suppressing a single unused-parameter / unused-variable warning the project policy requires.
  - **Not allowed in this change**: changing the `Decoder → VisitNamedInstruction → leaf` execution flow; restructuring `Simulator`, `Decoder`, or any imported subsystem; rewriting leaf semantics; introducing a predecode cache or IR; removing or replacing constructors / members on imported classes.
- Preserve every imported file's VIXL copyright header verbatim. Update `AUTHORS` to credit upstream and reproduce the upstream `LICENCE` text (e.g., as `LICENSE.vixl`).
- Compile imported sources with `VIXL_INCLUDE_TARGET_A64` and `VIXL_INCLUDE_SIMULATOR_AARCH64` defined. `VIXL_DEBUG` follows `CMAKE_BUILD_TYPE`.
- Narrow the project's compile-flag policy so VIXL sources build cleanly under their own warning expectations **without** weakening warnings for the rest of the project.
- Add one tiny **runtime smoke** (single test binary, ~20 LOC) that:
  1. Constructs a `Simulator` instance, exercising the imported subsystem constructors (decoder graph, disassembler, debugger, CPU features auditor).
  2. Executes a single NOP through the imported `Decoder → visitor → leaf` path without crash or assertion failure.
  This is the "**it ran**" signal — not a baseline correctness test, not a benchmark, not an API surface.
- Preserve the existing `gaby_vm_smoke` test and the `version.cc` build path. `ctest` runs both smokes after this change.

No JIT. No RWX memory. No runtime code generation. iOS/macOS/POSIX portability constraints in `CLAUDE.md` are respected. The `../vixl/` working tree remains reference-only — Gaby-VM gains no runtime dependency on it.

## Capabilities

### New Capabilities

- `aarch64-simulator`: Execute AArch64 user-mode (EL0) instructions on a software interpreter derived from the VIXL simulator. In this change, the capability is delivered at "imports compile, link, and a `Simulator` can construct + run one instruction" — the foundation for downstream capabilities (real baseline tests, benchmark harness, dispatch cache, embedding API), each landing as its own change.

### Modified Capabilities

None — this is the first capability in the project.

## Impact

- **Source tree**: imported VIXL files land under `src/` (shared) and `src/aarch64/` (simulator-specific), coexisting with the existing `src/version.cc.in`. Exact namespacing / public-header strategy is settled in `design.md`.
- **Build**: `src/CMakeLists.txt` extended to compile the imported sources. A VIXL-scoped flag set in `cmake/CompileFlags.cmake` lets upstream code build without project-wide warning relaxation.
- **Tests**: one new test binary (e.g., `simulator_smoke`) registered in `test/CMakeLists.txt` alongside the existing `gaby_vm_smoke`. Both run under `ctest`.
- **License/attribution**: every imported file keeps its VIXL header; `AUTHORS` credits upstream contributors; the upstream `LICENCE` text ships with the repo.
- **Edit-marker convention**: every Gaby-VM modification inside an imported file is bracketed by a comment (specific form chosen in `design.md`). This is the durable mechanism for finding our delta when upstream changes — a single grep for the marker enumerates every drifted location.
- **External dependencies**: none added. `../vixl/` is reference-only, not consumed by the build.
- **Defines applied to VIXL sources only**: `VIXL_INCLUDE_TARGET_A64`, `VIXL_INCLUDE_SIMULATOR_AARCH64`, and `VIXL_DEBUG` (debug builds).
- **Explicitly out of scope** for this change (each becomes a follow-up if needed):
  - A real baseline correctness test driving multi-instruction sequences.
  - The benchmark harness from `docs/refs/baseline-benchmark-suite.md`.
  - Any public Gaby-VM embedding API beyond what the smoke test needs.
  - The decode-cache scaffolding.
  - aarch32, the macro-assembler, the assembler, code generation, anything in Tier 0 of the extraction map.
