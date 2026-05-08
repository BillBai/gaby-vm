## ADDED Requirements

### Requirement: Import scope is bounded to extraction-map Tiers 1, 2, and 3

The repository SHALL contain VIXL source files only from Tiers 1, 2, and 3 of `docs/refs/vixl-extraction-map.md`. Tier 0 files (assembler, macro-assembler, code-buffer, pool-manager, AArch32, build tooling, and the helpers listed under "Tier 0 — out of scope") SHALL NOT be present in the imported tree.

#### Scenario: Tier 1 file is present at the expected path
- **WHEN** an entry from Tier 1 of the extraction map is checked (e.g., `src/aarch64/simulator-aarch64.cc`, `src/utils-vixl.h`)
- **THEN** the file exists in the repository at the corresponding path under `src/`

#### Scenario: Tier 0 file is absent from the repository
- **WHEN** an entry from Tier 0 of the extraction map is checked (e.g., `assembler-aarch64.cc`, anything under `aarch32/`, `code-buffer-vixl.cc`, `pool-manager.h`)
- **THEN** no file with that name exists anywhere under `src/`

#### Scenario: AArch32 directory is not present
- **WHEN** the path `src/aarch32/` is checked
- **THEN** the directory does not exist

### Requirement: Imported file layout mirrors upstream `src/` layout exactly

Imported VIXL files SHALL be placed at paths that mirror their upstream location relative to `../vixl/src/`. Shared root files (e.g., `utils-vixl.h`, `globals-vixl.h`, `cpu-features.h`) live directly under `src/`. AArch64-specific files live under `src/aarch64/`. No additional directory levels (such as `src/vixl/`, `third_party/vixl/`, or `src/sim/`) SHALL be introduced for imported files.

#### Scenario: Shared root file is at the upstream-relative path
- **WHEN** the imported path of `utils-vixl.h` is compared with `../vixl/src/utils-vixl.h`
- **THEN** the imported path is `src/utils-vixl.h` (same relative path under `src/`)

#### Scenario: AArch64 file is at the upstream-relative path
- **WHEN** the imported path of `simulator-aarch64.cc` is compared with `../vixl/src/aarch64/simulator-aarch64.cc`
- **THEN** the imported path is `src/aarch64/simulator-aarch64.cc` (same relative path under `src/`)

#### Scenario: No `third_party/vixl/` tree exists for imported files
- **WHEN** the path `third_party/vixl/` is checked
- **THEN** it does not exist (or, if `third_party/.gitkeep` remains, no VIXL files are nested under it)

### Requirement: Imported files are byte-identical to upstream except at marked locations

Each imported file's content SHALL match its upstream counterpart in `../vixl/` byte-for-byte, except inside regions that are bracketed by Gaby-VM marker comments. Marker comments are the *only* legitimate source of drift from upstream.

#### Scenario: Unmodified file matches upstream byte-for-byte
- **WHEN** an imported file containing no `gaby-vm` marker is compared (`diff`) against its upstream counterpart
- **THEN** the diff is empty

#### Scenario: Modified file matches upstream outside marker regions
- **WHEN** an imported file containing one or more `gaby-vm` markers is compared against upstream
- **THEN** every differing line either *is* a marker comment, or lies between a `// gaby-vm BEGIN` / `// gaby-vm END` pair, or is the single line immediately following a `// gaby-vm:` single-line marker

### Requirement: All edits to imported files use the documented marker convention

Any deviation from upstream content inside an imported file SHALL be bracketed by a comment marker in one of two forms:

- Single-line edit: a `// gaby-vm: <reason>` comment on the line immediately above the modified line.
- Multi-line edit (including deletions): a `// gaby-vm BEGIN: <reason>` line above the changed region and a `// gaby-vm END` line below it. Removed code is left commented out within the block so the deletion is reviewable.

The marker token `gaby-vm` SHALL be lowercase. A single grep for `gaby-vm` across `src/` SHALL enumerate every modified location.

#### Scenario: Single-line edit is preceded by a single-line marker
- **WHEN** a single line of an imported file differs from upstream
- **THEN** the line immediately above it is a `// gaby-vm: <reason>` comment

#### Scenario: Multi-line edit is bracketed by BEGIN/END markers
- **WHEN** two or more contiguous lines of an imported file differ from upstream
- **THEN** the line immediately above the region is a `// gaby-vm BEGIN: <reason>` comment and the line immediately below is `// gaby-vm END`

#### Scenario: Marker grep enumerates every drifted location
- **WHEN** running `git grep -nE 'gaby-vm( BEGIN| END|:)' src/`
- **THEN** the output includes at least one match for every region that differs from upstream

### Requirement: Imported simulator code is preserved structurally

The imported decode → visitor → leaf execution flow SHALL be preserved with no structural changes. Specifically:

- Class declarations for `Simulator`, `Decoder`, `Instruction`, and the rest of the imported subsystem SHALL retain their upstream member variables, constructor signatures, and method declarations.
- The `Decoder → VisitNamedInstruction → leaf` dispatch flow SHALL be invoked unchanged.
- The `vixl::aarch64` namespace SHALL be preserved (no rename to `gaby_vm::aarch64` or other namespace).
- No predecode cache, IR, or alternative dispatch path SHALL be introduced by this change.

#### Scenario: Imported `Simulator` class declaration matches upstream
- **WHEN** `src/aarch64/simulator-aarch64.h`'s `Simulator` class declaration is compared with upstream
- **THEN** the public, protected, and private member variables and method signatures match (additions are permitted only inside marker-commented regions; removals are not permitted)

#### Scenario: `vixl::aarch64` namespace is unchanged
- **WHEN** `git grep -n 'namespace vixl' src/` is run
- **THEN** the imported files declare and use the `vixl` and `vixl::aarch64` namespaces (no renames to `gaby_vm` or other namespace introduced)

#### Scenario: No predecode cache is introduced
- **WHEN** the imported simulator's instruction-execution path is traced
- **THEN** it goes `Decoder → VisitNamedInstruction → leaf`, with no intervening cache lookup, IR materialization, or alternative dispatch

### Requirement: VIXL preprocessor defines are PRIVATE to `gaby_vm`

The VIXL build defines `VIXL_INCLUDE_TARGET_A64` and `VIXL_INCLUDE_SIMULATOR_AARCH64` SHALL be defined when compiling `gaby_vm` sources. `VIXL_DEBUG` SHALL be defined when `CMAKE_BUILD_TYPE` is `Debug`. None of these defines SHALL propagate to consumers of `gaby_vm` via `INTERFACE` properties. The defines `VIXL_INCLUDE_TARGET_A32`, `VIXL_INCLUDE_TARGET_A32_T32`, `VIXL_CODE_BUFFER_MMAP`, and `VIXL_CODE_BUFFER_MALLOC` SHALL NOT be defined.

#### Scenario: VIXL defines are present when compiling `gaby_vm` sources
- **WHEN** an imported `.cc` file is compiled (e.g., inspecting `compile_commands.json`)
- **THEN** the compile command contains `-DVIXL_INCLUDE_TARGET_A64` and `-DVIXL_INCLUDE_SIMULATOR_AARCH64`

#### Scenario: `VIXL_DEBUG` follows `CMAKE_BUILD_TYPE`
- **WHEN** the project is configured with `CMAKE_BUILD_TYPE=Debug`
- **THEN** the compile command for an imported source contains `-DVIXL_DEBUG`
- **WHEN** the project is configured with `CMAKE_BUILD_TYPE=Release`
- **THEN** the compile command for an imported source does NOT contain `-DVIXL_DEBUG`

#### Scenario: Defines do not leak to consumers
- **WHEN** a downstream target links against `gaby_vm::gaby_vm` and inspects its inherited definitions
- **THEN** none of the VIXL defines appear in the consumer's compile commands

#### Scenario: Out-of-scope defines are not set
- **WHEN** any source in the project is compiled
- **THEN** the compile command does NOT contain `-DVIXL_INCLUDE_TARGET_A32`, `-DVIXL_INCLUDE_TARGET_A32_T32`, `-DVIXL_CODE_BUFFER_MMAP`, or `-DVIXL_CODE_BUFFER_MALLOC`

### Requirement: Warning-policy boundary is enforced via per-file flags

Imported source files SHALL compile under the warning relaxation provided by `gaby_vm_apply_imported_compile_flags` in `cmake/CompileFlags.cmake`. Project-authored sources SHALL continue to compile under `gaby_vm_apply_compile_flags` (the existing helper) without modification of that helper's flag set. The relaxation set SHALL retain `-Wall` and `-Wextra`, and SHALL consist only of `-Wno-*` additions; `-w` and removals of `-Wall`/`-Wextra` SHALL NOT be used. Each `-Wno-*` flag in the helper SHALL have an inline comment naming the warning class it suppresses.

#### Scenario: Imported sources build with relaxed flags
- **WHEN** `cmake --build` is run for the `dev-debug` and `dev-release` presets
- **THEN** all imported sources compile without errors, using the relaxed flag set

#### Scenario: Project-authored helper is unchanged
- **WHEN** `cmake/CompileFlags.cmake`'s existing `gaby_vm_apply_compile_flags` body is inspected
- **THEN** its flag list remains `-Wall -Wextra -Wpedantic` (no relaxations added)

#### Scenario: Imported helper documents each suppression
- **WHEN** `cmake/CompileFlags.cmake`'s `gaby_vm_apply_imported_compile_flags` body is inspected
- **THEN** every `-Wno-*` flag listed has an inline comment explaining what triggers it

### Requirement: License headers, license file, and attribution are preserved

Per-file VIXL copyright headers in every imported file SHALL remain unmodified. The upstream license text SHALL be available at `LICENSE.vixl` at the repo root, byte-identical to `../vixl/LICENCE`. The top-level `LICENSE` SHALL reference `LICENSE.vixl`. The `AUTHORS` file SHALL credit VIXL contributors and reference the upstream project.

#### Scenario: Imported file retains its VIXL copyright header
- **WHEN** an imported file's first 25 lines are read
- **THEN** they contain the unmodified VIXL copyright header (matching the upstream file's header byte-for-byte)

#### Scenario: `LICENSE.vixl` matches upstream license text
- **WHEN** `LICENSE.vixl` is compared with `../vixl/LICENCE`
- **THEN** the two files match byte-for-byte

#### Scenario: Top-level `LICENSE` references the imported license
- **WHEN** `LICENSE` is read
- **THEN** it contains text referencing `LICENSE.vixl` and indicating that portions of the project are derived from VIXL

#### Scenario: `AUTHORS` credits VIXL contributors
- **WHEN** `AUTHORS` is read
- **THEN** it contains a section crediting VIXL contributors and referencing the upstream VIXL project

### Requirement: Simulator constructs and executes a single NOP

A test binary `simulator_smoke` SHALL be registered with CTest and SHALL succeed by constructing a `vixl::aarch64::Simulator` instance, presenting it with a single AArch64 NOP instruction (`0xd503201f`), executing that instruction through the imported decode → visitor → leaf path, and exiting with status 0.

#### Scenario: `simulator_smoke` is registered with CTest
- **WHEN** `ctest -N` is run from the build directory
- **THEN** the output includes a test named `simulator_smoke`

#### Scenario: `simulator_smoke` passes
- **WHEN** `ctest -R simulator_smoke` is run
- **THEN** the test passes (exit code 0, no assertion failure, no crash)

#### Scenario: NOP execution exercises the decode + leaf path
- **WHEN** `simulator_smoke` is executed
- **THEN** the simulator's `Decoder` decodes the NOP, dispatches via the visitor map, and the corresponding leaf returns without error

### Requirement: The existing `gaby_vm_smoke` test continues to pass

The pre-existing `gaby_vm_smoke` test SHALL continue to pass after this change without modification of its source code or its CTest registration.

#### Scenario: `gaby_vm_smoke` source is unchanged
- **WHEN** `test/smoke.cc` is compared against its pre-change content
- **THEN** the file is unchanged

#### Scenario: `gaby_vm_smoke` passes
- **WHEN** `ctest -R '^smoke$'` (or the equivalent) is run
- **THEN** the test passes

### Requirement: Public header surface does not expose VIXL types

Files under `include/gaby_vm/` SHALL NOT include any imported VIXL header (e.g., `aarch64/simulator-aarch64.h`, `utils-vixl.h`) and SHALL NOT reference any `vixl::*` symbol in declarations. Imported simulator types SHALL be reachable only via PRIVATE include access granted to internal targets (such as `simulator_smoke`).

#### Scenario: No public header includes a VIXL header
- **WHEN** `git grep -n '#include' include/gaby_vm/` is run
- **THEN** no result references an imported VIXL header path

#### Scenario: No public header references the `vixl` namespace
- **WHEN** `git grep -n 'vixl::' include/gaby_vm/` is run
- **THEN** the output is empty

#### Scenario: Smoke test reaches imported headers via PRIVATE include
- **WHEN** the CMake build settings for `simulator_smoke` are inspected
- **THEN** the target has a PRIVATE include directory pointing at `${PROJECT_SOURCE_DIR}/src` (granting access to imported headers without exposing them publicly)
