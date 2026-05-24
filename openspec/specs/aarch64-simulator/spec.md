# aarch64-simulator Specification

## Purpose
Execute AArch64 user-mode (EL0) instructions on a software interpreter derived
from the VIXL simulator. This capability owns the imported simulator code
(located in `src/` and `src/aarch64/`), the build-time boundary between project
warning policy and imported-source warning policy, the marker convention for
auditable deviations from upstream, and the test contract that proves the
interpreter can decode and execute instructions end-to-end. It is the
foundation that downstream capabilities — the predecode/dispatch cache, the
embedding API, and any platform-portability work — will refine and extend.
## Requirements
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
- **THEN** every differing line is one of: a marker comment line (a `// gaby-vm:`, `// gaby-vm BEGIN:`, or `// gaby-vm END` token line, or one of the ordinary `//` reason lines that immediately follow a `// gaby-vm:` or `// gaby-vm BEGIN:` token line); a line that lies between a `// gaby-vm BEGIN:` / `// gaby-vm END` pair; or the single changed line immediately below a `// gaby-vm:` marker block

### Requirement: All edits to imported files use the documented marker convention

Any deviation from upstream content inside an imported file SHALL be bracketed by a comment marker in one of two forms. In both forms the marker *token* SHALL occupy its own line, and the reason text SHALL follow on the next line(s) as ordinary `//` comments:

- Single-line edit: a `// gaby-vm:` token line, followed by one or more ordinary `//` reason lines, placed immediately above the single modified line.
- Multi-line edit (including deletions): a `// gaby-vm BEGIN:` token line followed by one or more ordinary `//` reason lines above the changed region, and a `// gaby-vm END` line below it. Removed code is left commented out within the block so the deletion is reviewable.

The marker token `gaby-vm` SHALL be lowercase. A token line SHALL carry no reason text after the token. A `// gaby-vm:` or `// gaby-vm BEGIN:` marker SHALL have at least one reason line. Reason lines SHALL NOT contain the literal sequences `gaby-vm:`, `gaby-vm BEGIN`, or `gaby-vm END`, so that they are not picked up by the enumeration grep. The command `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` SHALL enumerate every modified location.

#### Scenario: Single-line edit is preceded by a single-line marker
- **WHEN** a single line of an imported file differs from upstream
- **THEN** the lines immediately above it are a `// gaby-vm:` token line carrying no inline reason, followed by one or more ordinary `//` reason lines

#### Scenario: Multi-line edit is bracketed by BEGIN/END markers
- **WHEN** two or more contiguous lines of an imported file differ from upstream
- **THEN** the region is preceded by a `// gaby-vm BEGIN:` token line carrying no inline reason, followed by one or more ordinary `//` reason lines, and the line immediately below the region is `// gaby-vm END`

#### Scenario: Marker token line carries no inline reason
- **WHEN** any `// gaby-vm:`, `// gaby-vm BEGIN:`, or `// gaby-vm END` token line in an imported file is inspected
- **THEN** the line contains only the marker token plus leading indentation, with the reason text (if any) on the following ordinary `//` lines

#### Scenario: Marker grep enumerates every drifted location
- **WHEN** running `git grep -nE 'gaby-vm( BEGIN| END|:)' src/`
- **THEN** the output includes at least one match for every region that differs from upstream, and ordinary `//` reason lines (which carry no marker token) are not matched

### Requirement: Imported simulator code is preserved structurally

The imported decode → visitor → leaf execution flow SHALL be preserved at the
leaf-semantic and namespace level. The marker convention (see the
*All edits to imported files use the documented marker convention* requirement)
remains the only legitimate source of drift from upstream.

Specifically:

- Class declarations for `Simulator`, `Decoder`, `Instruction`, and the rest
  of the imported subsystem SHALL retain their upstream member variables,
  constructor signatures, and method declarations. Removing or renaming an
  existing member is NOT permitted.
- Structural additions (new methods, fields, helper classes) on imported
  types ARE permitted when they follow the marker convention AND when each
  marker reason names the design document that motivates the addition.
- The leaf `VisitXxx` and `Simulate_*` member functions, plus the
  `LogicAArch64` helpers they call, SHALL preserve upstream semantics. Any
  marker-bracketed deviation in a leaf body SHALL document why the deviation
  cannot be expressed at a higher layer.
- The imported `Decoder → VisitNamedInstruction → leaf` dispatch flow SHALL
  remain reachable from the simulator. A non-cached execution path (e.g.,
  for tracing, debugging, shadow self-test, or bring-up) SHALL be available
  that exercises this flow.
- The `vixl::aarch64` namespace SHALL be preserved (no rename to
  `gaby_vm::aarch64` or other namespace).
- Alternate dispatch paths (e.g., a predecode cache) MAY be introduced
  alongside the imported flow, provided each addition is marker-bracketed and
  references a design document.

#### Scenario: Imported `Simulator` class declaration matches upstream

- **WHEN** `src/aarch64/simulator-aarch64.h`'s `Simulator` class declaration is compared with upstream
- **THEN** the public, protected, and private member variables and method signatures match (additions are permitted only inside marker-commented regions; removals are not permitted)

#### Scenario: `vixl::aarch64` namespace is unchanged

- **WHEN** `git grep -n 'namespace vixl' src/` is run
- **THEN** the imported files declare and use the `vixl` and `vixl::aarch64` namespaces (no renames to `gaby_vm` or other namespace introduced)

#### Scenario: Imported dispatch flow remains reachable

- **WHEN** any non-cached execution path through the simulator is exercised (e.g., a `DebugRunFrom` entry, a tracing loop, or any path that calls `Decoder::Decode`)
- **THEN** the path goes through the imported `Decoder → VisitNamedInstruction → leaf` flow
- **AND** alternative dispatch paths (predecode cache, etc.) MAY exist alongside, each introduced with marker comments whose reason text references a design document under `docs/refs/`

#### Scenario: Marker reason cites a design document for additive structural changes

- **WHEN** an imported file contains a `// gaby-vm:` or `// gaby-vm BEGIN` marker that introduces a new method, field, or helper class
- **THEN** the marker's reason text contains a path or filename matching `docs/refs/<doc>.md` (or an equivalent in-tree design document)

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

### Requirement: `simulator_correctness` test is registered with CTest using the privileged build pattern

The repository SHALL include a CTest-registered executable named
`simulator_correctness`, built from a single project-authored translation unit
under `test/`, that links to `gaby_vm::gaby_vm` and gains private access to
imported VIXL headers via the same build pattern `simulator_smoke` already uses:

- `target_link_libraries(simulator_correctness PRIVATE gaby_vm::gaby_vm)`.
- `target_include_directories(simulator_correctness PRIVATE ${PROJECT_SOURCE_DIR}/src)`.
- `target_compile_definitions(simulator_correctness PRIVATE VIXL_INCLUDE_TARGET_A64 VIXL_INCLUDE_SIMULATOR_AARCH64 $<$<CONFIG:Debug>:VIXL_DEBUG>)`.
- `gaby_vm_apply_compile_flags(simulator_correctness)` (the project-policy
  helper, NOT the imported-source relaxation helper).

The CTest case SHALL be named `simulator_correctness` and SHALL be wired with
`add_test(NAME simulator_correctness COMMAND simulator_correctness)`. The test
SHALL pass under both the `dev-debug` and `dev-release` configurations. This
change SHALL introduce no edits to files under `src/` and no edits to public
headers under `include/gaby_vm/`.

#### Scenario: `simulator_correctness` is enumerated by CTest

- **WHEN** running `ctest -N` (or `ctest --show-only`) in a configured build directory
- **THEN** a test case named `simulator_correctness` appears in the listing alongside `smoke` and `simulator_smoke`

#### Scenario: Test passes under the `dev-debug` preset

- **WHEN** running `cmake --preset dev-debug && cmake --build --preset dev-debug && ctest --preset dev-debug -R '^simulator_correctness$'`
- **THEN** the test exits with status 0

#### Scenario: Test passes under the `dev-release` preset

- **WHEN** running `cmake --preset dev-release && cmake --build --preset dev-release && ctest --preset dev-release -R '^simulator_correctness$'`
- **THEN** the test exits with status 0

#### Scenario: Privileged build pattern matches `simulator_smoke`

- **WHEN** the `add_executable(simulator_correctness …)` block in `test/CMakeLists.txt` is compared against the existing `add_executable(simulator_smoke …)` block
- **THEN** the third-party include (`PRIVATE ${PROJECT_SOURCE_DIR}/src`), the three VIXL compile definitions (`VIXL_INCLUDE_TARGET_A64`, `VIXL_INCLUDE_SIMULATOR_AARCH64`, and `$<$<CONFIG:Debug>:VIXL_DEBUG>`), and the compile-flags helper (`gaby_vm_apply_compile_flags`) are identical between the two blocks

#### Scenario: No imported `src/` file is modified by this change

- **WHEN** `git diff --name-only <merge-base>..HEAD -- src/` is run on the change branch
- **THEN** the output is empty

#### Scenario: Public header surface is unchanged by this change

- **WHEN** `git diff --name-only <merge-base>..HEAD -- include/` is run on the change branch
- **THEN** the output is empty

### Requirement: `simulator_correctness` exercises baseline AArch64 instruction families and asserts on post-`RunFrom` state

The `simulator_correctness` test SHALL drive hand-encoded AArch64 instruction
sequences through `vixl::aarch64::Simulator::RunFrom` and SHALL assert on
post-execution register and/or memory state against precomputed expected
values. Sequences SHALL be expressed as raw `uint32_t` arrays in test source
(no in-tree assembler, no instruction-emit helper macros). Each sequence SHALL
terminate via a trailing `RET` to a NULL link register, relying on the
`LR == kEndOfSimAddress` invariant established by `Simulator::ResetRegisters`.

The test SHALL include at least one passing sub-test for each of the following
instruction families, where each sub-test sets up its inputs via
`Simulator::WriteXRegister` (or `WriteWRegister`) and reads its outputs via
`Simulator::ReadXRegister` (or `ReadWRegister`) and/or direct host-buffer reads:

- **Integer arithmetic** — at minimum `ADD`, `SUB`, `MUL`.
- **Logical** — at minimum `AND`, `ORR`, `EOR`.
- **Load/store** — at minimum `LDR` and `STR`, exercised with **both** an
  immediate-offset addressing mode and a register-offset addressing mode.
- **Conditional control flow** — at minimum `B.cond`, `CBZ`, `CBNZ`. The
  `B.cond` sub-test SHALL be preceded in its sequence by an explicit
  flag-setting instruction (e.g. `SUBS` or `CMP`) so the branch outcome is
  determined by observed NZCV state rather than by `ResetSystemRegisters`'s
  default values.
- **Procedure call / return** — `BL` followed by `RET`. The sub-test SHALL
  exercise the inner call's effect on `LR` and SHALL preserve the outer
  `LR == NULL` terminator across the inner call so that the outer `RET`
  terminates the simulation. Observable post-run state SHALL prove both that
  control entered the callee and that the inner `RET` returned to the
  instruction immediately following the `BL`.

A run in which any sub-test's actual post-`RunFrom` state differs from its
precomputed expected value SHALL fail the CTest case (non-zero exit status)
with diagnostic output identifying which sub-test failed and the actual vs.
expected values.

#### Scenario: Integer arithmetic family is exercised

- **WHEN** the `simulator_correctness` source is inspected
- **THEN** at least one sub-test exercises each of `ADD`, `SUB`, and `MUL`, asserting on a register read whose expected value is the host-computed result of the inputs

#### Scenario: Logical family is exercised

- **WHEN** the `simulator_correctness` source is inspected
- **THEN** at least one sub-test exercises each of `AND`, `ORR`, and `EOR`, asserting on a register read whose expected value is the host-computed result of the inputs

#### Scenario: Load/store family is exercised with both addressing modes

- **WHEN** the `simulator_correctness` source is inspected
- **THEN** at least one sub-test exercises `LDR` with an immediate-offset addressing mode, at least one exercises `LDR` with a register-offset addressing mode, at least one exercises `STR` with an immediate-offset addressing mode, and at least one exercises `STR` with a register-offset addressing mode (any sub-test MAY combine `LDR` and `STR` so long as both modes are covered overall)

#### Scenario: Conditional control flow family is exercised with both taken and not-taken paths

- **WHEN** the `simulator_correctness` source is inspected
- **THEN** at least one sub-test exercises each of `B.cond`, `CBZ`, and `CBNZ` with paired taken and not-taken cases (or equivalent coverage that distinguishes both paths), and the `B.cond` sub-test's sequence contains an explicit flag-setting instruction (e.g. `SUBS` or `CMP`) immediately before the `B.cond` encoding

#### Scenario: Procedure call/return family is exercised

- **WHEN** the `simulator_correctness` source is inspected
- **THEN** at least one sub-test contains a `BL` followed (after at least the inner callee's body and inner `RET`) by an outer `RET`, and the sub-test's post-run assertions verify both that the callee's body executed (via observable register or memory state written only inside the callee) and that the simulation terminated normally through the outer `RET`

#### Scenario: Sequences are raw `uint32_t` arrays, not assembled at runtime

- **WHEN** `git grep -nE 'class [A-Z][A-Za-z]*Assembler|MacroAssembler|EmitInstruction' test/simulator_correctness.cc` is run
- **THEN** the output is empty (no assembler or emit-helper machinery is present in the test source)

#### Scenario: Each sequence terminates via `RET` to NULL `LR`

- **WHEN** the encoded sequences in `simulator_correctness` are inspected
- **THEN** every sequence ends with a `RET` encoding (`0xd65f03c0` for `RET X30`) and no sub-test calls `WriteLr` to override the `LR == kEndOfSimAddress` initial state established by `Simulator::ResetRegisters`

#### Scenario: Failing sub-test fails the CTest case with a diagnostic

- **WHEN** any sub-test's actual post-`RunFrom` state differs from its expected value
- **THEN** the test process exits with non-zero status, the captured stdout identifies the failing sub-test by name, and the captured stdout reports both the actual and expected values

### Requirement: Public register-identifier header is provided

The repository SHALL contain a public header `include/gaby_vm/registers.h`
declaring the strongly-typed register identifiers and FFI-stable register-data
types used by the `gaby_vm::Simulator` typed register-I/O surface. In the
`gaby_vm` namespace the header SHALL declare:

- `enum class GpRegister : uint8_t`, with enumerators `X0`..`X30` taking
  underlying values 0..30, an `LR` enumerator that is an alias for `X30`,
  an `SP` enumerator with underlying value 31, and a `PC` enumerator with
  underlying value 32.
- `enum class VRegister : uint8_t`, with enumerators `V0`..`V31` taking
  underlying values 0..31.
- `enum class SysRegister : uint8_t`, with enumerators `NZCV`, `FPCR`,
  `FPSR`, and `BType`.
- `struct VRegisterValue { uint64_t lo; uint64_t hi; }`, moved from
  `simulator.h` so the typed surface and `RegisterFile` can refer to it
  through `registers.h`.
- `struct RegisterFile` (see the "RegisterFile is a POD with frozen layout"
  requirement).
- `struct GpWrite { GpRegister reg; uint64_t value; }`,
  `struct VWrite { VRegister reg; VRegisterValue value; }`,
  `struct SysWrite { SysRegister reg; uint32_t value; }`, and
  `using RegisterWrite = std::variant<GpWrite, VWrite, SysWrite>`.

The header SHALL NOT include any imported VIXL header and SHALL NOT reference
any `vixl::*` symbol. It SHALL be self-contained: a translation unit that
`#include`s only `<gaby_vm/registers.h>` (and no other gaby-vm header) SHALL
compile.

#### Scenario: Header exists at the public-API path

- **WHEN** the path `include/gaby_vm/registers.h` is checked
- **THEN** the file exists

#### Scenario: Enums declare a `uint8_t` underlying type

- **WHEN** `include/gaby_vm/registers.h` is parsed for the declarations of
  `GpRegister`, `VRegister`, and `SysRegister`
- **THEN** each declaration has the form `enum class <name> : uint8_t`

#### Scenario: `GpRegister` covers X0..X30, LR, SP, PC with the documented codes

- **WHEN** the enumerator values of `gaby_vm::GpRegister` are inspected
- **THEN** `X0` through `X30` have underlying values 0 through 30
  respectively, `LR` has the same underlying value as `X30`, `SP` has
  underlying value 31, and `PC` has underlying value 32

#### Scenario: No VIXL include or symbol leaks into `registers.h`

- **WHEN** `git grep -nE 'vixl|aarch64/' include/gaby_vm/registers.h` is run
- **THEN** the output is empty

#### Scenario: Header is self-contained

- **WHEN** a translation unit `#include`s only `<gaby_vm/registers.h>` and
  no other gaby-vm header
- **THEN** the translation unit compiles without error

### Requirement: Simulator exposes typed register Read/Write accessors

`gaby_vm::Simulator` SHALL expose the following member functions in
`include/gaby_vm/simulator.h`. The Read overloads SHALL be `const`-qualified:

- `void Write(GpRegister reg, uint64_t value);` and
  `uint64_t Read(GpRegister reg) const;`
- `void Write(VRegister reg, VRegisterValue value);` and
  `VRegisterValue Read(VRegister reg) const;`
- `void Write(SysRegister reg, uint32_t value);` and
  `uint32_t Read(SysRegister reg) const;`

Each typed `Write` SHALL update the same backing register slot that the
corresponding instruction-level semantic write reaches; each typed `Read`
SHALL return the current value of that slot. A `Write` immediately followed
by a `Read` of the same register identifier on the same `Simulator` (with no
instruction executed in between) SHALL return the value just written.

The `GpRegister::PC` case of `Write(GpRegister, uint64_t)` SHALL retain the
re-entrancy contract previously attached to `WritePc`: it MAY be used to seat
a top-level entry point before a `StepOnce()` / `DebugStepOnce()` loop, but
MUST NOT be used to seat a *nested* step from inside a leaf executed by an
enclosing run. Re-entrant callers SHALL seat a nested entry point through
`RunFrom`, `StepOnce(entry_pc)`, or `DebugStepOnce(entry_pc)`. The docstring
on `Write(GpRegister, uint64_t)` SHALL state this constraint explicitly for
the PC case and SHALL name the three re-entrant alternatives.

#### Scenario: GP register write round-trips through Read

- **WHEN** a caller invokes `sim.Write(GpRegister::X3, 0xdeadbeefdeadbeef)`
  and then `sim.Read(GpRegister::X3)` without executing any instruction in
  between
- **THEN** the `Read` returns `0xdeadbeefdeadbeef`

#### Scenario: SP write reaches the SP slot, not XZR

- **WHEN** a caller invokes `sim.Write(GpRegister::SP, 0x1234567890abcdef)`
  and then `sim.Read(GpRegister::SP)`
- **THEN** the `Read` returns `0x1234567890abcdef`
- **AND** the value is observable as the simulator's stack pointer (not as
  the XZR-encoded slot 31)

#### Scenario: V register write preserves both 64-bit halves

- **WHEN** a caller invokes
  `sim.Write(VRegister::V5, VRegisterValue{0x1111111122222222, 0x3333333344444444})`
  and then `sim.Read(VRegister::V5)`
- **THEN** the `Read` returns
  `VRegisterValue{lo = 0x1111111122222222, hi = 0x3333333344444444}`

#### Scenario: System-register typed write round-trips

- **WHEN** a caller invokes `sim.Write(SysRegister::NZCV, 0x30000000)` and
  then `sim.Read(SysRegister::NZCV)`
- **THEN** the `Read` returns `0x30000000`
- **AND** equivalent round-trips for `FPCR`, `FPSR`, and `BType` also return
  the values just written

#### Scenario: PC write seats the cursor for a subsequent step

- **WHEN** a caller invokes `sim.Write(GpRegister::PC, entry_pc)` and then
  `sim.StepOnce()` (with no other state mutation in between)
- **THEN** the instruction at `entry_pc` is the one executed by `StepOnce`

#### Scenario: PC docstring documents the nested-step hazard

- **WHEN** the docstring attached to `Simulator::Write(GpRegister, uint64_t)`
  in `include/gaby_vm/simulator.h` is read
- **THEN** it states that the `GpRegister::PC` case MUST NOT be used to seat
  a nested step from inside a leaf, and names `RunFrom`,
  `StepOnce(entry_pc)`, and `DebugStepOnce(entry_pc)` as the re-entrant
  alternatives

### Requirement: Existing unsigned-coded register accessors are removed

`gaby_vm::Simulator`'s public surface in `include/gaby_vm/simulator.h` SHALL
NOT declare any of:

- `WriteXRegister(unsigned, uint64_t)` or `ReadXRegister(unsigned) const`,
- `WriteSp(uint64_t)` or `ReadSp() const`,
- `WritePc(uintptr_t)` or `ReadPc() const`,
- `ReadVRegister(unsigned) const`.

These entry points SHALL be wholly absent — they MAY NOT be retained as
deprecated forwarders. Internal call sites in the project (under `test/`,
`src/gaby_vm/`, and any in-tree benchmark target) SHALL be migrated to the
typed surface; the build SHALL NOT compile if any caller still names them.

#### Scenario: Removed accessor names are absent from the public header

- **WHEN** `git grep -nE '\b(WriteXRegister|ReadXRegister|WriteSp|ReadSp|WritePc|ReadPc|ReadVRegister)\b' include/gaby_vm/simulator.h` is run
- **THEN** the output is empty

#### Scenario: Project compiles with the removed accessors absent

- **WHEN** the project is configured and built (e.g.
  `cmake --preset dev-debug && cmake --build --preset dev-debug`)
- **THEN** the build succeeds end-to-end, demonstrating that no in-tree
  caller still references the removed entry points

### Requirement: `RegisterFile` is a POD with frozen layout

`include/gaby_vm/registers.h` SHALL declare `struct RegisterFile` with this
exact field order and exact field types:

| order | field   | type                  |
|-------|---------|-----------------------|
| 1     | `x`     | `uint64_t[31]`        |
| 2     | `sp`    | `uint64_t`            |
| 3     | `pc`    | `uint64_t`            |
| 4     | `v`     | `VRegisterValue[32]`  |
| 5     | `nzcv`  | `uint32_t`            |
| 6     | `fpcr`  | `uint32_t`            |
| 7     | `fpsr`  | `uint32_t`            |
| 8     | `btype` | `uint32_t`            |

`RegisterFile` SHALL be a standard-layout, trivially-copyable POD. A
`static_assert` in `registers.h` SHALL fix
`sizeof(RegisterFile) == 31*8 + 8 + 8 + 32*16 + 4*4`. The struct SHALL NOT
contain any `uintptr_t`, `size_t`, or `bool` field, so its layout is
independent of host pointer width.

#### Scenario: Field order matches the spec

- **WHEN** the declaration of `gaby_vm::RegisterFile` in
  `include/gaby_vm/registers.h` is inspected
- **THEN** the fields appear in the order `x`, `sp`, `pc`, `v`, `nzcv`,
  `fpcr`, `fpsr`, `btype`, with the types listed above

#### Scenario: `sizeof(RegisterFile)` is frozen at compile time

- **WHEN** `include/gaby_vm/registers.h` is searched for a `static_assert`
  on `sizeof(RegisterFile)`
- **THEN** the file contains a `static_assert` that asserts
  `sizeof(RegisterFile) == 31*8 + 8 + 8 + 32*16 + 4*4` (or an arithmetically
  equivalent literal such as `792`)

#### Scenario: `RegisterFile` is standard-layout and trivially copyable

- **WHEN** a translation unit evaluates
  `std::is_standard_layout_v<gaby_vm::RegisterFile> &&
   std::is_trivially_copyable_v<gaby_vm::RegisterFile>`
- **THEN** the expression is `true`

### Requirement: `ReadAll` / `WriteAll` snapshot and restore the full guest state

`gaby_vm::Simulator` SHALL expose:

- `RegisterFile ReadAll() const;`
- `void WriteAll(const RegisterFile& file);`

`ReadAll` SHALL populate every field of the returned `RegisterFile` from the
current value of the corresponding register slot. `WriteAll` SHALL update
every register slot from the corresponding field of `file`, with the same
observable effect as a sequence of individual typed `Write` calls that
covers each slot exactly once.

`WriteAll` is a top-level state-restore entry point. It SHALL NOT be called
from inside a leaf executed by an enclosing run; its docstring SHALL state
this constraint explicitly.

#### Scenario: ReadAll → WriteAll round-trips the architectural state

- **WHEN** a caller takes `auto snap = sim.ReadAll();`, executes any number
  of instructions, calls `sim.WriteAll(snap);`, and then takes
  `sim.ReadAll()` again
- **THEN** the second `ReadAll` result equals `snap` field-by-field

#### Scenario: WriteAll covers every architectural slot

- **WHEN** a caller constructs a `RegisterFile` with deliberately distinct
  values in every `x[i]`, `sp`, `pc`, `v[i]`, and sysreg field, invokes
  `sim.WriteAll(file)`, and then reads each slot back via the typed `Read`
  overloads
- **THEN** each typed `Read` returns the value the caller placed in the
  matching `RegisterFile` field

#### Scenario: WriteAll docstring documents the top-level-only constraint

- **WHEN** the docstring attached to `Simulator::WriteAll` in
  `include/gaby_vm/simulator.h` is read
- **THEN** it states that `WriteAll` is for top-level use only and MUST NOT
  be called from inside a leaf executed by an enclosing run

### Requirement: Batch write accepts a `std::span<const RegisterWrite>`

`gaby_vm::Simulator` SHALL expose a public member function
`void Write(std::span<const RegisterWrite> writes);` where
`RegisterWrite = std::variant<GpWrite, VWrite, SysWrite>` as declared in
`registers.h`.

Each element of `writes` SHALL be applied in span order. Applying an element
SHALL update the same backing slot, with the same value semantics, that the
matching single-element typed `Write` overload would update — selected by the
active variant alternative. The span write SHALL behave as the sequence of
typed `Write` calls that the variant alternatives encode: there is no
atomic-commit guarantee, and entries that ran before an aborting one stay
applied.

If an element carries an enum value whose underlying integer is outside the
declared range for that enum (for instance a `GpRegister` produced by
`static_cast<GpRegister>(99)`), the implementation SHALL abort with a
diagnostic that identifies the offending element's position in the span.

#### Scenario: Mixed GP / V / Sys batch is applied in order

- **WHEN** a caller invokes
  `sim.Write(std::array<RegisterWrite, 3>{
      GpWrite{GpRegister::X0, 1},
      VWrite{VRegister::V0, VRegisterValue{2, 3}},
      SysWrite{SysRegister::NZCV, 0x40000000},
   });`
- **THEN** `sim.Read(GpRegister::X0)` returns `1`,
  `sim.Read(VRegister::V0)` returns `VRegisterValue{2, 3}`, and
  `sim.Read(SysRegister::NZCV)` returns `0x40000000`

#### Scenario: Out-of-range enum value aborts the batch

- **WHEN** a caller invokes `sim.Write(...)` with a span containing an
  element whose `GpRegister` was constructed via
  `static_cast<GpRegister>(99)`
- **THEN** the call aborts with a diagnostic that identifies the offending
  element's position

#### Scenario: `RegisterWrite` resolves to the documented variant

- **WHEN** the declaration of `gaby_vm::RegisterWrite` in
  `include/gaby_vm/registers.h` is inspected
- **THEN** it is declared as
  `using RegisterWrite = std::variant<GpWrite, VWrite, SysWrite>;`

### Requirement: Simulator constructor rejects undersized stack buffers

The `gaby_vm::Simulator(PredecodeCache*, void* stack_buffer, size_t stack_size)` constructor SHALL reject `stack_size` values strictly less than a documented minimum, exposed as the public `static constexpr` member `gaby_vm::Simulator::kMinStackSize`. On rejection the constructor SHALL abort via `VIXL_ABORT_WITH_MSG` (or equivalent) with a diagnostic that names both the rejected `stack_size` and the `kMinStackSize` value. The minimum SHALL be greater than or equal to the sum of the imported `vixl::aarch64::SimStack` default `limit_guard_size_` plus `usable_size_` (currently `4 * 1024 + 8 * 1024 = 12288` bytes in `src/aarch64/simulator-aarch64.h`). `stack_size` values at or above the minimum SHALL construct a usable `Simulator` whose initial guest SP is set to the 16-byte-aligned top of `stack_buffer`, exactly as today.

#### Scenario: Below-minimum stack size aborts with a diagnostic

- **WHEN** a `gaby_vm::Simulator` is constructed with `stack_size = 0`, `stack_size = 16`, or any value strictly less than `gaby_vm::Simulator::kMinStackSize`
- **THEN** the constructor aborts before returning, and the abort diagnostic includes both the literal text "kMinStackSize" and a decimal representation of the rejected `stack_size`

#### Scenario: At-minimum stack size constructs normally

- **WHEN** a `gaby_vm::Simulator` is constructed with `stack_size == gaby_vm::Simulator::kMinStackSize`
- **THEN** the constructor returns a usable `Simulator` whose initial guest SP, observed via `Read(GpRegister::SP)`, equals the 16-byte-aligned top of `stack_buffer`

#### Scenario: kMinStackSize matches or exceeds the VIXL SimStack default total

- **WHEN** the value of `gaby_vm::Simulator::kMinStackSize` is compared with the default value of `vixl::aarch64::SimStack::limit_guard_size_` plus `vixl::aarch64::SimStack::usable_size_` in `src/aarch64/simulator-aarch64.h`
- **THEN** `kMinStackSize` is greater than or equal to that sum, enforced at compile time by a `static_assert` in `src/gaby_vm/simulator.cc`
