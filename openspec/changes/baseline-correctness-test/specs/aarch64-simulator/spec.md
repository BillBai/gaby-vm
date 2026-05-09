## ADDED Requirements

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
