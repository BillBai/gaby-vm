## 1. License, attribution, and upstream baseline

- [x] 1.1 Copy `../vixl/LICENCE` to `LICENSE.vixl` at the repo root, byte-identical to upstream
- [x] 1.2 Update top-level `LICENSE` with a short note pointing to `LICENSE.vixl` and indicating that portions of the project are derived from VIXL
- [x] 1.3 Update `AUTHORS` with a "VIXL contributors" section that points to the upstream VIXL project
- [x] 1.4 Record the VIXL upstream commit hash being imported (in `AUTHORS` or a top-level `NOTES.vixl` file) so future re-sync efforts have a clear baseline

## 2. Build-system scaffolding (no imported files yet)

- [x] 2.1 Add `gaby_vm_apply_imported_compile_flags(<file_list>)` helper to `cmake/CompileFlags.cmake` with an empty `-Wno-*` body and an inline comment block explaining its purpose; do not modify `gaby_vm_apply_compile_flags`
- [x] 2.2 In `src/CMakeLists.txt`, add `target_compile_definitions(gaby_vm PRIVATE VIXL_INCLUDE_TARGET_A64 VIXL_INCLUDE_SIMULATOR_AARCH64)`
- [x] 2.3 In `src/CMakeLists.txt`, add `target_compile_definitions(gaby_vm PRIVATE $<$<CONFIG:Debug>:VIXL_DEBUG>)`
- [x] 2.4 Declare a `GABY_VM_IMPORTED_SOURCES` CMake variable in `src/CMakeLists.txt` (initially empty); wire it through `target_sources(gaby_vm PRIVATE ${GABY_VM_IMPORTED_SOURCES})` and `gaby_vm_apply_imported_compile_flags(${GABY_VM_IMPORTED_SOURCES})`
- [x] 2.5 Suppress `clang-format` for imported files: per W2 verify-first, the project's clang-format 22.1.3 produces small drift (binary-operator spacing in template expressions) on upstream VIXL files, so suppression is needed. `src/.clang-format` with `DisableFormat: true` covers `src/` and (via clang-format's directory walk) `src/aarch64/`. Confirmed zero drift on sample
- [x] 2.6 Verify the build still succeeds (`cmake --preset dev-debug && cmake --build build/debug && ctest --preset dev-debug`) before any imports — confirms scaffolding is harmless

## 3. Import VIXL files (Tier 1+2+3 starting set)

- [x] 3.1 Copy Tier 1 shared-root files from `../vixl/src/` to `src/`: `globals-vixl.h`, `platform-vixl.h`, `utils-vixl.h`, `utils-vixl.cc`, `compiler-intrinsics-vixl.h`, `compiler-intrinsics-vixl.cc`, `cpu-features.h`, `cpu-features.cc`
- [x] 3.2 Copy Tier 1 simulator core from `../vixl/src/aarch64/` to `src/aarch64/`: `simulator-aarch64.h`, `simulator-aarch64.cc`, `simulator-constants-aarch64.h`, `logic-aarch64.cc`, `pointer-auth-aarch64.cc`
- [x] 3.3 Copy Tier 1 decoder + instruction metadata: `decoder-aarch64.h`, `decoder-aarch64.cc`, `decoder-constants-aarch64.h`, `decoder-visitor-map-aarch64.h`, `instructions-aarch64.h`, `instructions-aarch64.cc`, `constants-aarch64.h`
- [x] 3.4 Copy Tier 1 CPU-features auditor: `cpu-features-auditor-aarch64.h`, `cpu-features-auditor-aarch64.cc`
- [x] 3.5 Copy Tier 1 registers / operands / ABI: `registers-aarch64.h`, `registers-aarch64.cc`, `operands-aarch64.h`, `operands-aarch64.cc`, `abi-aarch64.h`
- [x] 3.6 Copy Tier 1 CPU model: `cpu-aarch64.h`, `cpu-aarch64.cc`
- [x] 3.7 Copy Tier 2 disassembler: `disasm-aarch64.h`, `disasm-aarch64.cc`
- [x] 3.8 Copy Tier 3 debugger: `debugger-aarch64.h`, `debugger-aarch64.cc`
- [x] 3.9 Append every imported file path to `GABY_VM_IMPORTED_SOURCES` in `src/CMakeLists.txt` (relative paths from `src/`)
- [x] 3.10 Verify per-file VIXL copyright headers are intact in every imported file (no edits, no insertions before the header)

## 4. Make the build pass

- [x] 4.1 Run `cmake --preset dev-debug` and capture the configure / first-build error output; categorize each failure as (a) missing header, (b) Tier-0 leak, or (c) warning-policy mismatch
- [x] 4.2 For category (a): import the missing file if it is in Tier 1/2/3 of the extraction map, appending it to `GABY_VM_IMPORTED_SOURCES`; flag any deviation from the extraction map for code review (none surfaced — Tier 1+2+3 was complete on first pass)
- [x] 4.3 For category (b): comment out the offending call site with a `// gaby-vm BEGIN: tier-0 <reason>` / `// gaby-vm END` block, leaving the original line(s) commented out so the deletion is reviewable; do NOT import Tier 0 files; if the removal would be structural, stop and escalate (`assembler-aarch64.h` include + `Instruction::Set{ImmPCOffsetTarget,PCRelImmTarget,BranchImmTarget,ImmLLiteral}` definitions in `instructions-aarch64.cc`)
- [x] 4.4 For category (c): determine the minimal `-Wno-*` set required for imported sources to compile under `-Wall -Wextra`; populate `gaby_vm_apply_imported_compile_flags`'s body with each flag, with a one-line inline comment naming what triggers it (one flag: `-Wno-deprecated-literal-operator` for `decoder-constants-aarch64.h`'s `operator"" _b`)
- [x] 4.5 Repeat 4.1–4.4 until both `cmake --preset dev-debug` and `cmake --preset dev-release` configure and build cleanly with no warnings escalated to errors
- [x] 4.6 Confirm `gaby_vm_smoke` continues to pass under both presets (`ctest --preset dev-debug -R '^smoke$'`)
- [x] 4.7 Run `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` and confirm every marker has a meaningful reason; rework any vague reasons (2 BEGIN/END pairs, both Tier-0 gates with explicit rationale)

## 5. Smoke test: construct simulator and execute one NOP

- [x] 5.1 Create `test/simulator_smoke.cc` that:
      a) constructs a `vixl::aarch64::Simulator` (with `CPUFeatures::All()` configured on its auditor),
      b) writes a single AArch64 NOP encoding (`0xd503201f`) into a buffer,
      c) drives that one instruction through the imported decode → visit → leaf path,
      d) returns 0 on success, non-zero on assertion / crash
- [x] 5.2 Register `simulator_smoke` in `test/CMakeLists.txt`: `add_executable(simulator_smoke simulator_smoke.cc)`, link to `gaby_vm::gaby_vm`, set `target_include_directories(simulator_smoke PRIVATE ${PROJECT_SOURCE_DIR}/src)`, `add_test(NAME simulator_smoke COMMAND simulator_smoke)`, apply `gaby_vm_apply_compile_flags(simulator_smoke)`. Note: the test target also needed `target_compile_definitions(simulator_smoke PRIVATE VIXL_INCLUDE_TARGET_A64 VIXL_INCLUDE_SIMULATOR_AARCH64 $<$<CONFIG:Debug>:VIXL_DEBUG>)` because `simulator-aarch64.h` is wrapped in `#ifdef VIXL_INCLUDE_SIMULATOR_AARCH64`; PRIVATE on the test mirrors the privileged include access and keeps the defines off external consumers.
- [x] 5.3 Run `ctest --preset dev-debug -R simulator_smoke` and confirm it passes
- [x] 5.4 Run `ctest --preset dev-debug` (no filter) and confirm BOTH `gaby_vm_smoke` and `simulator_smoke` pass; repeat for `dev-release`

## 6. Verification against the spec

- [x] 6.1 **R1 (import scope)**: confirm no Tier 0 file path exists under `src/` (e.g., grep for `assembler-aarch64`, `macro-assembler`, `code-buffer`, `pool-manager`, `aarch32`); confirm Tier 1 files listed in the extraction map are all present
- [x] 6.2 **R2 (layout mirrors upstream)**: for each imported file, confirm its path under `src/` matches its upstream path under `../vixl/src/`
- [x] 6.3 **R3 (byte-identical except at marked locations)**: for every imported file, run `diff <upstream> <imported>` and confirm every differing line is either a marker comment, inside a `// gaby-vm BEGIN/END` block, or the line directly under a `// gaby-vm:` single-line marker (only `instructions-aarch64.cc` drifts; drift is wholly inside two BEGIN/END blocks)
- [x] 6.4 **R4 (marker convention)**: review every `gaby-vm` marker for proper form (single-line vs. block, lowercase, has reason); confirm `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` enumerates every drift
- [x] 6.5 **R5 (no structural edits)**: spot-check `Simulator`, `Decoder`, and `Instruction` class declarations against upstream; confirm member variables, constructor signatures, and method declarations match (additions only inside marker regions; no removals or renames); confirm `vixl::aarch64` namespace is preserved (`simulator-aarch64.h` byte-identical to upstream)
- [x] 6.6 **R6 (defines PRIVATE)**: inspect `compile_commands.json` for `gaby_vm` sources under both presets; confirm `VIXL_INCLUDE_TARGET_A64` and `VIXL_INCLUDE_SIMULATOR_AARCH64` are present, `VIXL_DEBUG` is present only under `dev-debug`, and none of the out-of-scope defines (`VIXL_INCLUDE_TARGET_A32`, etc.) are set; confirm `gaby_vm`'s `INTERFACE_COMPILE_DEFINITIONS` is empty for VIXL macros (verified via direct compile_commands.json inspection: `gaby_vm_smoke` sees zero VIXL defines)
- [x] 6.7 **R7 (warning-policy boundary)**: confirm `gaby_vm_apply_compile_flags` body is unchanged (`-Wall -Wextra -Wpedantic`); confirm `gaby_vm_apply_imported_compile_flags` retains `-Wall -Wextra` and uses only `-Wno-*` additions, each with an inline comment (one flag in use: `-Wno-deprecated-literal-operator`)
- [x] 6.8 **R8 (license preserved)**: `diff LICENSE.vixl ../vixl/LICENCE` returns empty; `LICENSE` references `LICENSE.vixl`; `AUTHORS` credits VIXL contributors; spot-check 3 imported files have unmodified VIXL copyright headers
- [x] 6.9 **R9 (NOP runs)**: `simulator_smoke` test passes (already covered by 5.3); confirm via test output that the simulator actually decoded and executed the NOP (not e.g. exiting before running the instruction) — output: `simulator_smoke: ran one NOP at <addr> (decoded + dispatched OK)`
- [x] 6.10 **R10 (existing smoke unchanged)**: `git diff test/smoke.cc` empty; `gaby_vm_smoke` passes
- [x] 6.11 **R11 (no VIXL in public surface)**: `git grep -n 'vixl::' include/gaby_vm/` returns empty; `git grep -n '#include' include/gaby_vm/` shows no imported VIXL header references; confirm `simulator_smoke` reaches imported headers only via its PRIVATE `target_include_directories`
- [x] 6.12 Run the project Stop hook's full build + tests once more under both presets; confirm green
