## MODIFIED Requirements

### Requirement: Import scope is bounded to extraction-map Tiers 1, 2, and 3

The **shipping import tree** (`Sources/gaby_vm/src/`) SHALL contain VIXL source files only from Tiers 1, 2, and 3 of `docs/refs/vixl-extraction-map.md`. Tier 0 files (assembler, macro-assembler, code-buffer, pool-manager, AArch32, build tooling, and the helpers listed under "Tier 0 — out of scope") SHALL NOT be present under `Sources/gaby_vm/src/`.

A single exception exists outside the shipping tree: a **test-only copy** of selected Tier 0 assembler files (the VIXL AArch64 assembler, macro-assembler, code-buffer, and their direct non-shared dependencies) MAY exist under `test/test_support/vixl_asm/`, used only to assemble instruction bytes at test time. That copy SHALL be pinned to the import SHA recorded in `docs/refs/vixl-extraction-map.md`, SHALL preserve each file's upstream VIXL license header, SHALL carry the test-only marker comment, and SHALL NOT be linked into `gaby_vm::gaby_vm` or any target that embeds it.

#### Scenario: Tier 1 file is present at the expected path
- **WHEN** an entry from Tier 1 of the extraction map is checked (e.g., `Sources/gaby_vm/src/aarch64/simulator-aarch64.cc`, `Sources/gaby_vm/src/utils-vixl.h`)
- **THEN** the file exists in the repository at the corresponding path under `Sources/gaby_vm/src/`

#### Scenario: Tier 0 file is absent from the shipping tree
- **WHEN** an entry from Tier 0 of the extraction map is checked (e.g., `assembler-aarch64.cc`, anything under `aarch32/`, `code-buffer-vixl.cc`, `pool-manager.h`)
- **THEN** no file with that name exists anywhere under `Sources/gaby_vm/src/`

#### Scenario: AArch32 directory is not present
- **WHEN** the path `Sources/gaby_vm/src/aarch32/` is checked
- **THEN** the directory does not exist

#### Scenario: Test-only Tier 0 copy is permitted outside the shipping tree
- **WHEN** the path `test/test_support/vixl_asm/` is checked
- **THEN** it MAY contain Tier 0 assembler files (e.g., `assembler-aarch64.cc`, `macro-assembler-aarch64.cc`, `code-buffer-vixl.cc`)
- **AND** each such file retains its upstream VIXL license header and carries the test-only marker comment
- **AND** no Tier 0 file appears under `Sources/gaby_vm/src/`

#### Scenario: The test-only copy does not link into the shipping library
- **WHEN** the link dependencies of `gaby_vm::gaby_vm` are inspected
- **THEN** no object compiled from `test/test_support/vixl_asm/` is present
- **AND** `Sources/gaby_vm/src/CMakeLists.txt` declares no `target_link_libraries`

### Requirement: VIXL preprocessor defines are PRIVATE to `gaby_vm`

The VIXL build defines `VIXL_INCLUDE_TARGET_A64` and `VIXL_INCLUDE_SIMULATOR_AARCH64` SHALL be defined when compiling `gaby_vm` sources. `VIXL_DEBUG` SHALL be defined when `CMAKE_BUILD_TYPE` is `Debug`. None of these defines SHALL propagate to consumers of `gaby_vm` via `INTERFACE` properties. When compiling `gaby_vm` sources, the defines `VIXL_INCLUDE_TARGET_A32`, `VIXL_INCLUDE_TARGET_A32_T32`, `VIXL_CODE_BUFFER_MMAP`, and `VIXL_CODE_BUFFER_MALLOC` SHALL NOT be defined.

The test-only assembler island (`test/test_support/vixl_asm/`) is the sole exception: its translation units SHALL be compiled with `VIXL_CODE_BUFFER_MALLOC` so the VIXL code buffer is a plain `malloc`/`free` buffer with no executable-memory path, and SHALL NOT be compiled with `VIXL_CODE_BUFFER_MMAP`. `VIXL_CODE_BUFFER_MALLOC` SHALL be applied `PRIVATE` to the island target only; it SHALL NOT appear on `gaby_vm::gaby_vm`, on any shipping source under `Sources/gaby_vm/src/`, or on any `INTERFACE`/`PUBLIC` property that could reach a `gaby_vm` consumer. `VIXL_CODE_BUFFER_MMAP` SHALL NOT be defined anywhere in the project.

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

#### Scenario: Out-of-scope defines are not set for shipping sources
- **WHEN** any `gaby_vm` shipping source under `Sources/gaby_vm/src/` is compiled
- **THEN** the compile command does NOT contain `-DVIXL_INCLUDE_TARGET_A32`, `-DVIXL_INCLUDE_TARGET_A32_T32`, `-DVIXL_CODE_BUFFER_MMAP`, or `-DVIXL_CODE_BUFFER_MALLOC`
- **WHEN** `VIXL_CODE_BUFFER_MMAP` is searched across the whole configured project
- **THEN** it is defined for no target

#### Scenario: The test-only island compiles with `VIXL_CODE_BUFFER_MALLOC`, never `VIXL_CODE_BUFFER_MMAP`
- **WHEN** a translation unit under `test/test_support/vixl_asm/` is compiled
- **THEN** the compile command contains `-DVIXL_CODE_BUFFER_MALLOC`
- **AND** it does NOT contain `-DVIXL_CODE_BUFFER_MMAP`
- **WHEN** a downstream target links `gaby_vm::gaby_vm` and inspects inherited definitions
- **THEN** `VIXL_CODE_BUFFER_MALLOC` does NOT appear

## ADDED Requirements

### Requirement: A test-only VIXL assembler island provides live assembly without leaking into the shipping library

The repository SHALL contain a CMake target named `gaby_vm_vixl_asm_testonly`, configured only when `GABY_VM_BUILD_TESTS` is enabled, built from a copy of the Tier 0 VIXL assembler files (assembler, macro-assembler, code-buffer, and the test infrastructure they require) taken at the import SHA recorded in `docs/refs/vixl-extraction-map.md`. The target SHALL link `gaby_vm::gaby_vm` `PRIVATE` and SHALL NOT declare a `::`-namespaced alias.

The island SHALL NOT re-copy any leaf header or source already present under `Sources/gaby_vm/src/` (`instructions`, `operands`, `registers`, `constants`, `cpu-features`, `utils`, `globals`, `compiler-intrinsics`). Its include path SHALL place `Sources/gaby_vm/src` first, so that includes from the copied assembler resolve to the already-imported leaf headers and to the imported dual-track simulator. Consequently each shared `vixl::` symbol SHALL be defined exactly once across the linked test binary.

#### Scenario: Island target is gated behind the test option
- **WHEN** the project is configured with `GABY_VM_BUILD_TESTS=OFF`
- **THEN** no `gaby_vm_vixl_asm_testonly` target is defined

#### Scenario: Island links the shipping library PRIVATE with no public alias
- **WHEN** the CMake definition of `gaby_vm_vixl_asm_testonly` is inspected
- **THEN** it links `gaby_vm::gaby_vm` with `PRIVATE` visibility
- **AND** no `add_library(... ALIAS ...)` introduces a `::` alias for it

#### Scenario: Shared leaf headers are not re-copied into the island
- **WHEN** the file names under `test/test_support/vixl_asm/` are listed
- **THEN** none of `instructions-aarch64.h`, `operands-aarch64.h`, `registers-aarch64.h`, `constants-aarch64.h`, `cpu-features.h`, `utils-vixl.h`, `globals-vixl.h`, or `compiler-intrinsics-vixl.h` appears

#### Scenario: No duplicate `vixl::` symbol definitions when linked with the shipping library
- **WHEN** the test binary that links both the island and `gaby_vm::gaby_vm` is examined with `nm`
- **THEN** no shared `vixl::` symbol is multiply defined
- **AND** no SVE symbol is left undefined

### Requirement: The assembler island contains no executable-memory path and never natively executes assembled bytes

No file under `test/test_support/vixl_asm/` SHALL have a live `#include <sys/mman.h>` or compile a call to `mmap`/`mprotect` to obtain executable memory. (Explanatory comments may *name* the header to document its deliberate absence, and the imported `code-buffer-vixl.cc` may retain upstream `mmap`/`mprotect` calls inside dead `#ifdef VIXL_CODE_BUFFER_MMAP` branches that the `VIXL_CODE_BUFFER_MALLOC` build never compiles — neither is a dependency.) VIXL's `test/test-utils.cc` (which mmaps RWX memory and natively calls assembled bytes via `ExecuteMemory`) SHALL NOT be copied into the island; a gaby-authored stub SHALL supply any symbol the test infrastructure links, with `ExecuteMemory` implemented as a no-op or abort-on-call. Upstream test bodies whose only need for the system mmap header is a simulated-`Mmap` / MTE path gaby does not model SHALL have that path excluded (a marked `#if 0`) so no live include remains. Assembled bytes SHALL be consumed only by the gaby simulator's decode path, never executed on the host CPU.

#### Scenario: The island carries no live system-mmap-header dependency
- **WHEN** `grep -rn '#include <sys/mman.h>' test/test_support/vixl_asm/` is run
- **THEN** the output is empty
- **AND** no translation unit compiles an `mmap`/`mprotect`/`munmap` call (the only such calls live in `code-buffer-vixl.cc`'s dead `VIXL_CODE_BUFFER_MMAP` branches, never compiled under `VIXL_CODE_BUFFER_MALLOC`)

#### Scenario: Upstream native-execution helper is not present
- **WHEN** the file names under `test/test_support/vixl_asm/` are listed
- **THEN** `test-utils.cc` is absent and a gaby-authored stub provides the linked symbols
- **AND** any `ExecuteMemory` symbol present resolves to a no-op or abort-on-call implementation

### Requirement: The `vixl_port` suite live-assembles upstream bodies and runs both tracks under absolute and differential oracles, covering memory access

The `vixl_port` CTest binaries SHALL, for each included upstream VIXL `TEST()` body, assemble that body at test time into a `malloc` code buffer using the island, then execute the assembled bytes on both the cache track (with a `PredecodeCache`) and the decoder track of `gaby_vm::Simulator`. Each body SHALL be verified against (a) an **absolute oracle** — the body-exit register file and memory image of a VIXL reference `Simulator`, read directly (not via `core.Dump`): each `ASSERT_EQUAL_*` / `ASSERT_NOT_EQUAL_*` register or flag SHALL be pinned to the reference's value, with each register operand read from its own bank (a vector register from the V file, an integer register from the X/sp file — an X and a V register sharing a code SHALL NOT be confused), and the body's exit store image SHALL match the reference's — and (b) a **differential oracle** — full `RegisterFile` equality AND exit-store-image equality between the cache track and the decoder track. Both tracks SHALL be seeded before each body to an entry state byte-equivalent to VIXL `Simulator::ResetState()`, with that equivalence asserted once at startup.

Bodies containing load/store, ADR/ADRP, literal-load, AND read-modify-write (atomic / exclusive / CAS, NEON store-multiple) instructions SHALL be included and SHALL run against real in-process memory; the **results** of their stores SHALL be verified, not merely executed, by comparing the body's exit memory image across all three engines (so a store the shared leaf gets wrong on both tracks is caught against the reference). The suite SHALL register with CTest under names matching `vixl_port` and SHALL pass under both the `dev-debug` and `dev-release` presets.

#### Scenario: `vixl_port` tests are enumerated and pass on both tracks
- **WHEN** `ctest -N` is run in a configured build directory
- **THEN** the listing contains test cases whose names match `vixl_port`
- **WHEN** `ctest --preset dev-debug -R vixl_port` and `ctest --preset dev-release -R vixl_port` are run
- **THEN** every selected test passes (exit 0, no oracle failure, no crash)

#### Scenario: A memory-access body is included and verified
- **WHEN** an upstream body that performs a load and a store through a register-held address is run
- **THEN** the body is included (not skipped) and assembles against the rig's real scratch buffer
- **AND** both tracks produce identical `RegisterFile` state (differential oracle)
- **AND** the post-run memory and register state match the VIXL reference sim (absolute oracle)

#### Scenario: Both tracks are seeded to a `ResetState`-equivalent entry state
- **WHEN** the suite starts
- **THEN** it asserts once that the seeded gaby entry state is field-equivalent to VIXL `Simulator::ResetState()`
- **AND** each body is run from that seeded state on both tracks, not from the simulator's default-constructed state

### Requirement: The suite retains capability-based skips and crash/hang safety nets and drops the structural filter

The suite SHALL keep the capability skip surface — the feature deny-list (e.g., MTE, BF16, PAuth, HBC) and the by-name isolation list (e.g., `printf`, `runtime_calls`, `dc_zva`, `system`, `mops`, `gcs_feature_off`) — and SHALL guard each body with a crash/hang signal handler plus a per-case instruction cap. A fatal signal or hang while a body is *assembling* or running under the *reference* sim SHALL be reported as a skip (an unportable body or a leaf the reference itself rejects). A fatal signal or hang while a body is running on a **gaby track** (cache or decoder) SHALL be reported as a **failure** — it is a dispatch / predecode regression, not a skip — and the suite MAY stop the remaining bodies in that family (the shared engine's re-entrancy state is unsound after the signal unwinds past its execution scope). In neither case SHALL the process abort uncontained. The suite SHALL NOT apply a structural load/store / PC-relative filter (the former `IsNonPortableInstr`).

The suite SHALL also pin a per-family expected ran/skipped count (a coverage baseline) and SHALL fail when the observed split drifts from it, so that a change which silently moves cases from "ran" to "skipped" is caught; an explicit opt-in (an environment variable) SHALL allow re-baselining. The baseline MAY differ between the debug and release configurations where a body's pass/skip legitimately depends on a debug-only assertion.

#### Scenario: A deny-listed body is skipped, not failed
- **WHEN** the suite encounters a body whose feature is on the deny-list or whose name is on the isolation list
- **THEN** the body is reported as skipped and the suite continues

#### Scenario: A crash during assembly or the reference run is contained as a skip
- **WHEN** a body raises a fatal signal or exceeds the per-case instruction cap while assembling or under the reference sim
- **THEN** the harness reports that body as skipped and continues running the remaining bodies without aborting the process

#### Scenario: A crash inside a gaby track fails the suite
- **WHEN** a body raises a fatal signal or hangs while executing on the cache track or the decoder track
- **THEN** the harness reports that body as a failure (not a skip) and the family's exit status is non-zero

#### Scenario: Coverage drift is caught
- **WHEN** a change moves one or more bodies between the ran and skipped sets without an explicit re-baseline
- **THEN** the affected family reports a coverage-drift failure and exits non-zero
- **WHEN** the re-baseline environment variable is set
- **THEN** the observed split is printed and the family does not fail on the drift

#### Scenario: No structural non-portability filter remains
- **WHEN** the suite's inclusion logic is inspected
- **THEN** there is no filter that drops a body solely because it contains a load/store, ADR/ADRP, or register-indirect branch

### Requirement: Frozen fixtures and the extraction tool are removed

After the migration completes, the repository SHALL NOT contain the frozen-fixture replay model — the committed fixtures under `test/vixl_port/generated/`, the fixture POD header `vixl_port_fixture.h`, or the replay runner `vixl_port_runner.{h,cc}` — and SHALL NOT contain the authorship-time extraction tool `tools/vixl_test_extract/` or its `GABY_VM_BUILD_VIXL_EXTRACT` CMake option. All `vixl_port` coverage SHALL come from live assembly at test time.

#### Scenario: Frozen-fixture artifacts are gone
- **WHEN** the repository is searched for `test/vixl_port/generated/`, `vixl_port_fixture.h`, and `vixl_port_runner.cc`
- **THEN** none of them exist

#### Scenario: The extraction tool and its build option are gone
- **WHEN** the repository is searched for `tools/vixl_test_extract/` and for the string `GABY_VM_BUILD_VIXL_EXTRACT` in any `CMakeLists.txt`
- **THEN** neither is found
- **AND** a clean configure of the `dev-debug` and `dev-release` presets succeeds with no dangling reference
