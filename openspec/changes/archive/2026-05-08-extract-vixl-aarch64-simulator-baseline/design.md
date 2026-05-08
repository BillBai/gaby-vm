## Context

The current Gaby-VM repository is a CMake skeleton: a top-level `CMakeLists.txt`, a single `gaby_vm` static library built from `src/version.cc.in`, a `gaby_vm_smoke` test that asserts `gaby_vm::version()` returns a non-empty string, and an empty `third_party/.gitkeep` placeholder. The simulator does not yet exist in this tree. The reference VIXL codebase lives at `../vixl/` (SCons-built) and is reference-only — Gaby-VM has no runtime dependency on it.

Per `CLAUDE.md`, **VIXL is the seed of Gaby-VM's simulator code, not a third-party dependency.** The simulator semantics we want are VIXL's, but the resulting files live in Gaby-VM's source tree (`src/`), evolve under Gaby-VM's CMake build, and will be modified (carefully, per the proposal) as the project develops the predecode/dispatch cache and embedding API. License headers and upstream attribution are preserved at the file and project level, but architecturally the imported code is Gaby-VM source from the moment it lands.

The proposal commits to importing VIXL files (Tiers 1+2+3 of `docs/refs/vixl-extraction-map.md`) such that the code **builds and runs** with no structural edits and a comment-marked, auditable delta against upstream. This document settles the architectural questions that flow from that contract: where files live, how CMake compiles them, the comment-marker convention, the smoke-test surface, and the risk-mitigation pattern.

Project constraints worth keeping front-of-mind (from `CLAUDE.md`):
- iOS / macOS / POSIX portable; **no JIT, no RWX memory, no runtime code generation**.
- Preserve VIXL license headers and attribution.
- "Optimize with measurements" — avoid premature abstraction; do not invent a new IR.

## Goals / Non-Goals

**Goals**

- Define a directory layout, rooted in `src/`, that lets imported VIXL files keep their upstream `#include` paths unmodified — and that frames the imported code as Gaby-VM source, not a vendored dependency.
- Define a CMake structure that compiles imported files as part of `gaby_vm` directly (one library, no extra targets), with VIXL-specific defines and warning relaxations applied only where actually needed.
- Pick a single, greppable comment-marker convention so every Gaby-VM modification of an imported file can be enumerated by `grep` in seconds.
- Define how the smoke test reaches the imported simulator without exposing VIXL types in the public `include/gaby_vm/` surface.
- Identify the small number of architectural risks (Tier-0 dependency leaks, warning-policy mismatch, auditor assertions) and document the mitigation pattern for each.

**Non-Goals**

- A baseline correctness test driving multi-instruction sequences (its own follow-up change).
- The benchmark harness from `docs/refs/baseline-benchmark-suite.md` (its own follow-up change).
- A public Gaby-VM embedding API beyond what the smoke test internally needs.
- Any decode-cache scaffolding, IR design, or leaf-semantics rewriting.
- Renaming VIXL's `vixl::aarch64` namespace to `gaby_vm::aarch64` — structural change, deferred.
- An automated VIXL re-sync tool — manual `grep`-based audit is sufficient for V1.
- Importing Tier 0 files (assembler, macro-assembler, code buffer, pool manager, aarch32, build tooling).

## Decisions

### D1. Imported files live in `src/`, mirroring upstream `src/` layout exactly

**Decision.** Imported VIXL files become Gaby-VM source code under `src/`, mirroring `../vixl/src/...` exactly:

- **Shared root** lands at `src/`: `src/utils-vixl.h/.cc`, `src/globals-vixl.h`, `src/cpu-features.h/.cc`, `src/compiler-intrinsics-vixl.h/.cc`, `src/platform-vixl.h`. These coexist with the existing `src/version.cc.in`.
- **AArch64-specific files** land at `src/aarch64/`: `simulator-aarch64.h/.cc`, `decoder-aarch64.h/.cc`, `instructions-aarch64.h/.cc`, and the rest of the Tier 1+2+3 set named in the extraction map.

`third_party/` is **not** used by this change. The existing `.gitkeep` placeholder may be removed (see Open Questions).

**Why.**
- VIXL is being adopted into Gaby-VM, not depended upon. The user-stated framing puts imported code in `src/` because that is where Gaby-VM source code lives.
- Upstream VIXL files cross-reference each other with paths like `"aarch64/foo.h"` and `"utils-vixl.h"` rooted at `src/`. Mirroring the layout means **every `#include` line stays unmodified** — a cornerstone of the no-structural-edits rule.
- Provenance is preserved through per-file VIXL copyright headers (untouched) and project-level attribution (D8).

**Alternatives considered.**
- `third_party/vixl/...` tree. Rejected per user direction: VIXL is not a third-party dependency here, and that framing would set up the wrong mental model for every subsequent change (predecode cache, dispatch tweaks, leaf optimizations) that *modifies* this code.
- Renaming files into Gaby-VM-style names (e.g., `simulator-aarch64.h` → `aarch64_simulator.h`). Rejected: structural change, hurts re-sync, no semantic gain.
- Adding an extra directory level like `src/sim/utils-vixl.h`. Rejected: would break upstream `#include` paths and force structural edits across nearly every imported file.

### D2. CMake: imported sources compile directly as part of `gaby_vm` (one library, no extra targets)

**Decision.** Imported `.cc` files are added to `gaby_vm` via `target_sources(gaby_vm PRIVATE …)`. There is **no separate static or OBJECT library** — imported code is `gaby_vm` from CMake's perspective. The imported file list is held in a CMake variable (e.g., `GABY_VM_IMPORTED_SOURCES`) so per-file compile-option overrides (D3) can target it cleanly.

`src/CMakeLists.txt` keeps its existing structure (`add_library(gaby_vm STATIC)`, the `target_include_directories(... PRIVATE ${PROJECT_SOURCE_DIR}/src)` line, the version-template configuration) and gains a single `target_sources` block listing the imported files.

**Why.**
- One library matches the framing: imported code *is* Gaby-VM code, not a sub-component.
- Consumers of `gaby_vm` continue to see exactly one static library — no second link target, no boundary to communicate.
- Per-file compile properties (D3) handle the warning-policy boundary without needing a separate CMake target.

**Alternatives considered.**
- OBJECT library `gaby_vm_simulator` aggregating imported sources. Rejected: introduces a target boundary that doesn't reflect the underlying reality, and `set_source_files_properties` solves the only problem the OBJECT library was solving (per-source flags).
- A second STATIC library. Rejected: even more strongly — exposes a second link target.

### D3. Compile-flag boundary via per-file properties; helper `gaby_vm_apply_imported_compile_flags`

**Decision.** `cmake/CompileFlags.cmake` gains a second helper:

```cmake
function(gaby_vm_apply_imported_compile_flags <file_list>)
  # Applies the warning relaxations the imported simulator code requires.
  # Project-authored code uses gaby_vm_apply_compile_flags() instead.
  # ...
endfunction()
```

The helper uses `set_source_files_properties(<files> PROPERTIES COMPILE_OPTIONS "<flags>")` to scope the relaxation to the listed files only. The relaxation set keeps `-Wall -Wextra` (still meaningful signal) and adds the *minimum* `-Wno-*` flags actually required for imported files to build under our project policy. **Each `-Wno-*` is documented inline in the helper** with a one-line comment naming what triggers it. The exact set is determined empirically during implementation.

The existing `gaby_vm_apply_compile_flags(target)` continues to govern project-authored code unchanged.

**Why.**
- `set_source_files_properties` is well-behaved under CMake 3.18+ (project requires 3.21), so the older directory-scope footgun is irrelevant here.
- Per-file scoping guarantees the relaxation does not leak to project-authored sources, even though both live inside the same `gaby_vm` target.
- A dedicated helper makes the boundary self-documenting at the CMake level — anyone reading `CompileFlags.cmake` sees the asymmetry and the rationale immediately.
- Restricting to `-Wno-*` flags (no `-w`, no removing `-Wall`/`-Wextra`) preserves the most signal possible.

**Alternatives considered.**
- Apply relaxations to the whole `gaby_vm` target. Rejected: weakens our own code's warnings, exactly what the proposal forbids.
- Use a separate OBJECT library to scope the flags. Rejected per D2: extra target boundary not justified.
- Suppress all warnings on imported files (`-w`). Rejected: loses real signal during re-sync.

### D4. VIXL preprocessor defines applied PRIVATE on `gaby_vm`

**Decision.** `target_compile_definitions(gaby_vm PRIVATE VIXL_INCLUDE_TARGET_A64 VIXL_INCLUDE_SIMULATOR_AARCH64)` and additionally `VIXL_DEBUG` in debug builds (gated by `$<CONFIG:Debug>`). PRIVATE so they don't leak to consumers; target-wide because the defines are inert in project-authored files (those files don't reference VIXL macros) and the simplicity is worth it.

**Out of scope** for this change — none of these are set, because the corresponding code is not imported: `VIXL_INCLUDE_TARGET_A32`, `VIXL_INCLUDE_TARGET_A32_T32`, `VIXL_CODE_BUFFER_MMAP`, `VIXL_CODE_BUFFER_MALLOC`.

**Why.**
- PRIVATE keeps the defines from leaking to consumers via INTERFACE propagation, which is the only consumer-visible concern.
- Target-wide is simpler than per-file scoping and harmless because non-VIXL files don't reference these macros.
- If a project-authored file ever did reference one of these macros (a smell to avoid), it would still resolve, but that's an issue catchable in code review — the CMake structure is not the right place to enforce it.

**Alternatives considered.**
- PUBLIC defines. Rejected: leaks VIXL implementation detail to consumers.
- Per-file definitions matching the imported source list. Rejected: more complex with no benefit since defines are inert in non-VIXL files.

### D5. Comment-marker convention: `// gaby-vm: <reason>` (single-line) and `// gaby-vm BEGIN: <reason>` / `// gaby-vm END` (multi-line)

**Decision.** Every Gaby-VM modification inside an imported file is bracketed by a comment marker. Two forms, both lowercase, both greppable with `grep -nE 'gaby-vm( BEGIN| END|:)'`:

- **Single-line edit** — one comment line *immediately above* the modified line:
  ```cpp
  // gaby-vm: <short reason>
  <modified line>
  ```
- **Multi-line / block edit** — explicit BEGIN / END:
  ```cpp
  // gaby-vm BEGIN: <short reason>
  <modified region>
  // gaby-vm END
  ```

Reasons are short fragments, action-oriented, and reference the underlying constraint: e.g., `port: clock_gettime fallback for iOS`, `build: silence Wshadow on shadowed loop var`, `tier-0: drop assembler-only call site`. **Removed code** uses BEGIN/END too, with the original line(s) commented out so the deletion is reviewable rather than invisible.

**Why.**
- A single greppable token (`gaby-vm`) is the durable mechanism to enumerate the delta against upstream. Anyone re-syncing types `git grep gaby-vm src/` and gets a complete list — no archaeology required.
- Single-line vs. multi-line forms reflect how edits naturally cluster: most port fixes are one line; most call-site removals are blocks. Forcing a single form would make either case awkward.
- The reason field is a forcing function: if you can't articulate the reason in a fragment, the edit probably isn't minimal enough.

**Alternatives considered.**
- Block-only (`BEGIN/END` for everything, including one-liners). Rejected: visually noisy.
- Single-line-only with implicit end. Rejected: ambiguous block scope; harder to review.
- Uppercase `GABY_VM`. Rejected: doesn't match the project's lowercase identifier conventions.
- Suffix marker on the same line (`<edit> // gaby-vm: <reason>`). Rejected: doesn't extend cleanly to multi-line, and conflicts with VIXL's own end-of-line comments.

### D6. Smoke test reaches `Simulator` via private `src/` include access — no public surface change

**Decision.** A new test `test/simulator_smoke.cc` is registered in `test/CMakeLists.txt` alongside the existing `gaby_vm_smoke`. It links to `gaby_vm` and additionally has `target_include_directories(simulator_smoke PRIVATE ${PROJECT_SOURCE_DIR}/src)` — granting the test, and only the test, the same internal-header access that `gaby_vm`'s own translation units have. Nothing in `include/gaby_vm/` changes.

The smoke does the minimum to prove "it ran": construct a `Simulator` instance (exercising the imported subsystem constructors — decoder graph, disassembler, debugger, CPU features auditor), prepare a tiny code buffer holding a single NOP (`0xd503201f`), invoke a single decode + execute step, return success.

**Why.**
- The proposal explicitly defers public embedding API. Exposing VIXL types in `include/gaby_vm/` would entrench coupling we may want to invert later.
- Granting the test PRIVATE access to `src/` mirrors what `gaby_vm`'s own internal code already sees. It's internally consistent and requires only one CMake line.
- Choosing a NOP minimizes setup: no memory model, no register state requirements, just decode → visit → leaf → no-op.

**Alternatives considered.**
- Add `gaby_vm/simulator.h` as a thin public wrapper. Rejected: premature API design; the smoke test doesn't require it.
- Move the smoke test into `src/` (or an `internal_test/`). Rejected: muddies the test/source boundary.
- Skip running the simulator and only construct it. Rejected: the user explicitly asked for "build and run" — running one instruction is the minimum that exercises decode + dispatch + a leaf.

### D7. Build-driven file list, anchored to the extraction map's tier rules

**Decision.** Import begins with the Tier 1 file list verbatim from `docs/refs/vixl-extraction-map.md`, plus the two Tier-2/Tier-3 files (`disasm-aarch64`, `debugger-aarch64`). When the build fails on a missing symbol or a missing header:

1. **If the missing artifact is in Tier 1/2/3**: add it to the import. Should be rare beyond a small handful of utility headers; the extraction map is exhaustive for the V1 surface.
2. **If the missing artifact is in Tier 0** (e.g., assembler internals reached via a debugger or disasm helper): **remove the call site** with a `// gaby-vm BEGIN: tier-0 <reason>` / `END` block, leaving the original code commented out so the deletion is reviewable. Do *not* import Tier 0.
3. **If the removal would be structural** (touches a constructor, member declaration, or dispatch flow): stop, escalate, and treat it as an open question for the spec / a possible scope adjustment.

**Why.** The proposal makes the file list "build-driven" — a contract that we don't try to predict the full dependency closure ahead of time. This decision pins down the *resolution policy* when the build surfaces a new dependency: prefer importing within tiers, prefer call-site removal over Tier-0 import, escalate structural decisions rather than silently making them.

### D8. License and attribution placement

**Decision.**
- Copy `../vixl/LICENCE` to `LICENSE.vixl` at the **repo root**, next to the existing `LICENSE`. Preserve spelling and content verbatim.
- Add a short note to the top-level `LICENSE` pointing to the imported license text: e.g., "Portions of this software are derived from VIXL — see `LICENSE.vixl`."
- Update `AUTHORS` with a section crediting VIXL contributors and pointing to the upstream repository.
- **Per-file VIXL copyright headers stay as-is in every imported file**, including any file we modify. Marker comments live *inside* the file body, not in the license header.

**Why.** With imported files now living in `src/` rather than a `third_party/vixl/` subtree, the VIXL license text needs an unambiguous home at the repo root. `LICENSE.vixl` is the conventional naming. Per-file headers continue to give in-place provenance for anyone reading individual files.

**Alternatives considered.**
- A `NOTICES` file. Acceptable but less directly discoverable than `LICENSE.vixl`.
- Inlining the VIXL license text into the project's existing `LICENSE`. Rejected: muddles the BSD-3-Clause project license with VIXL's separate license text.

## Risks / Trade-offs

- **[Tier-0 dependency leak]** — A Tier-1/2/3 file references something in Tier 0 (e.g., a debugger helper that pulls in an assembler symbol). → **Mitigation**: D7's resolution policy (prefer call-site removal with `tier-0` reason marker; escalate structural cases). Expected to hit a small number of helpers; documented at implementation time in marker comments.

- **[Warning-policy mismatch]** — Imported sources may emit warnings the project policy treats as errors. → **Mitigation**: D3's per-file relaxation, with each `-Wno-*` documented inline in the helper. Risk of regression: future re-sync may bring new warnings; covered by manual review.

- **[Re-sync friction]** — Future VIXL upstream changes may need merging back. → **Mitigation**: D5's marker convention is the durable mechanism. No automated tool in V1; a manual procedure can be added later if re-sync becomes a real workflow.

- **[macOS / iOS portability]** — VIXL has platform-specific paths (notably `VIXL_CODE_BUFFER_MMAP` using `mprotect(PROT_EXEC)`, W^X-incompatible on iOS). → **Mitigation**: code-buffer is Tier 0; not imported. Other platform paths in imported files should be portable; verified empirically by the smoke test running on macOS during implementation. iOS cross-check deferred to a follow-up.

- **[Auditor assertion]** — `simulator-aarch64.cc` asserts on `cpu_features_auditor_.InstructionIsAvailable()` (~line 1441). If the smoke test's NOP triggers this with default config, the smoke fails. → **Mitigation**: configure the auditor with `CPUFeatures::All()` (or the host feature set) at smoke-test setup. If still firing, that's a real bug worth surfacing rather than papering over.

- **[Default-constructed `Debugger` / `PrintDisassembler` cost]** — `Simulator`'s constructor allocates a disabled debugger and a print disassembler bound to a `FILE*` stream. → **Mitigation**: accept the cost in V1; their default-disabled state means runtime overhead is small. Stripping them is structural — postponed.

- **[Build-driven file list = unknown final manifest]** — We can't say *exactly* which files end up in `src/` until implementation. → **Mitigation**: the extraction map is exhaustive enough that surprises should be small. The actual file list goes into `tasks.md` once known, and a deviation from the extraction map is a flag-worthy event during code review.

## Migration Plan

This is a greenfield import — no existing simulator to migrate from, no rollback complexity beyond `git revert`. No deployment phasing needed.

## Open Questions

1. **`vixl::aarch64` namespace** — preserved verbatim in V1 (renaming is structural). Worth flagging as a future decision once the project's public API solidifies; will likely come up alongside the eventual embedding-API change.
2. **Smoke-test execution path** — The smallest "run one NOP" sequence depends on VIXL's `Simulator` API surface (e.g., `RunFrom<>`, `ExecuteInstruction`, manual PC advance). Settled in `tasks.md` once the import is sitting in-tree.
3. **Per-source compile-flag overrides** — If most warnings can be killed via the helper but one or two files need `set_source_files_properties` overrides on top, that's an acceptable late-binding decision.
4. **`third_party/.gitkeep`** — With no third-party content in this change, does the placeholder stay (anchoring future genuine deps) or get removed? Trivial either way; not load-bearing.
