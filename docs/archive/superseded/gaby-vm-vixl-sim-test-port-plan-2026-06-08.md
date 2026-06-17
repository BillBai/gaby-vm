# VIXL Simulator Test Port - Implementation Plan - 2026-06-08

> **Archived plan.** This plan implemented the design in
> [`gaby-vm-vixl-sim-test-port-design-2026-06-08.md`](./gaby-vm-vixl-sim-test-port-design-2026-06-08.md).
> It has since been superseded by the live-assembly `vixl_port` harness.
>
> The original Chinese task-by-task plan is preserved in
> `gaby-vm-vixl-sim-test-port-plan-2026-06-08.zh-cn.md`.

## Goal

Port the VIXL AArch64 execution-style tests from
`test-assembler-aarch64.cc`, `test-assembler-fp-aarch64.cc`, and
`test-assembler-neon-aarch64.cc` into gaby-vm as a correctness guardrail before
aggressive dispatch optimization.

## Architecture

The plan used a two-stage model:

- Authorship-time extraction: a developer-only tool links `../vixl` and uses
  macro redefinition to run upstream VIXL test bodies through the real
  `MacroAssembler` and `Simulator`.
- Shipping replay: committed generated fixtures are replayed by a gaby-vm-only
  harness on both cache and decoder tracks.

The generated fixture for each case contains instruction bytes, the entry
register state, harvested assertion targets, and feature metadata. The replay
harness applies both a differential oracle and an absolute oracle.

## Key API Facts Used By The Plan

The plan depended on the public gaby-vm API:

- `Simulator(PredecodeCache* cache, void* stack_buffer, size_t stack_size)`.
- `RunFrom(uintptr_t entry_pc)` for cache-track execution.
- `DebugRunFrom(uintptr_t entry_pc)` for decoder-track execution.
- `Write`, `Read`, `WriteAll`, and `ReadAll` for register state.
- `RegisterFile`, `GpRegister`, `VRegisterValue`, and `SysRegister`.
- `PredecodeCache::RegisterCodeRange`.

It also depended on `test/embedding_stack.h` for a 16 KiB aligned stack and on
the existing `test/simulator_correctness.cc` dual-run pattern.

## Planned File Layout

Developer-only extraction tool:

- `tools/vixl_test_extract/capture_macros.h`
- `tools/vixl_test_extract/capture_state.{h,cc}`
- `tools/vixl_test_extract/fixture_writer.{h,cc}`
- `tools/vixl_test_extract/extract_main.cc`
- `tools/vixl_test_extract/phase0_sample_tests.cc`
- `tools/vixl_test_extract/CMakeLists.txt`

Self-contained replay tests:

- `test/vixl_port/vixl_port_fixture.h`
- `test/vixl_port/vixl_port_runner.{h,cc}`
- `test/vixl_port/generated/*_fixtures.inc`
- `test/vixl_port/generated/manifest_*.md`
- `test/vixl_port/vixl_port_integer.cc`
- `test/vixl_port/vixl_port_fp.cc`
- `test/vixl_port/vixl_port_neon.cc`
- `test/vixl_port/CMakeLists.txt`

Top-level build wiring:

- Add `GABY_VM_BUILD_VIXL_EXTRACT`.
- Conditionally add `tools/vixl_test_extract`.
- Always add the self-contained replay tests.

## Phase 0: Prove The Pipeline

Phase 0 intentionally covered only a tiny integer slice. It was meant to prove
the end-to-end flow before scaling the extraction system.

Tasks:

1. Add POD fixture types in `test/vixl_port/vixl_port_fixture.h`.
2. Write a replay runner API in `vixl_port_runner.h`.
3. Add a hand-authored fixture, such as `add x0, x1, x2; ret`, to drive the
   runner before the extraction tool exists.
4. Build a failing test first, then implement `vixl_port_runner.cc` so the
   hand-authored fixture passes.
5. Inject a wrong expected value to prove the guardrail fails when it should.
6. Add the extraction-tool skeleton and capture macros.
7. Add Phase 0 VIXL-style sample tests.
8. Generate `phase0_extracted.inc`.
9. Replace the hand-written fixture with extracted fixtures and pass replay.

Expected Phase 0 acceptance:

- Default build passes without `../vixl` or the extraction option.
- Extraction build can generate fixtures when `../vixl` is available.
- A deliberate assertion mismatch fails the suite.

## Phase 1: Integer, Logical, Memory, And Branch Cases

The next phase would include `test-assembler-aarch64.cc` under the capture
macros.

Tasks:

1. Add a source file that includes `capture_macros.h`, then includes the real
   VIXL `test-assembler-aarch64.cc`.
2. Resolve macro conflicts and unsupported helpers by extending capture macros
   or quarantining specific cases.
3. Generate `integer_fixtures.inc` and `manifest_integer.md`.
4. Switch `vixl_port_integer.cc` to the generated fixture set.
5. Run `ctest -R vixl_port_integer --output-on-failure`.
6. Triage failures:
   - process aborts from unimplemented leaves become quarantined with reasons;
   - cache/decoder mismatches are real bugs or regressions;
   - address-dependent absolute assertions should be skipped and reported;
   - literal-pool and data-in-stream cases may require quarantine or product
     support.
7. Commit fixtures and manifest only after the test is green.

Acceptance: integer replay is green and the manifest lists included and skipped
cases with reasons.

## Phase 2: Floating Point

Phase 2 extends state capture and assertions for FP tests.

Tasks:

1. Capture vector register halves and FPCR/FPSR where needed.
2. Implement `RecordFP32` and `RecordFP64`.
3. Compare FP values by raw bits, including NaNs.
4. Add `vixl_port_fp.cc` and CMake registration.
5. Generate `fp_fixtures.inc` and `manifest_fp.md`.
6. Run `ctest -R vixl_port_fp --output-on-failure`.

Acceptance: FP replay is green, and FPCR-sensitive cases are either correctly
captured or explicitly skipped.

## Phase 3: NEON

Phase 3 extends capture and checking to 128-bit vector assertions.

Tasks:

1. Confirm the real `QRegisterValue` half-access API in `../vixl`.
2. Capture `v_lo` and `v_hi`.
3. Implement `RecordV128`.
4. Add `vixl_port_neon.cc` and CMake registration.
5. Generate `neon_fixtures.inc` and `manifest_neon.md`.
6. Run `ctest -R vixl_port_neon --output-on-failure`.

Acceptance: all three `vixl_port_*` tests are registered in CTest and the
default build remains self-contained.

## Final Validation

The plan ended with:

1. A clean default build and full `ctest` run without `../vixl`.
2. Documentation in `docs/testing.md` explaining the source of `vixl_port`,
   how to refresh fixtures, and how to read manifests.
3. A deliberate cache-track defect to prove the guardrail fails.
4. A final commit with fixtures, manifests, and docs.

## Coverage Against The Design

The plan mapped to the design as follows:

- Broad coverage: separate integer, FP, and NEON fixture sets.
- Authorship-time byte extraction: capture macros plus real VIXL test `.cc`
  inclusion under an opt-in tool target.
- Dual oracle: full register-file differential comparison plus harvested
  assertion checks.
- Address relocation: differential comparison is relocation-safe; address-like
  absolute assertions are skipped and reported.
- Filtering: feature deny lists, case quarantine, and explicit manifests.
- Build separation: Tier-0 VIXL appears only when extraction is enabled.
- Non-goals: no SVE, no trace matrix, no runtime assembler, and no leaf semantic
  rewrite.

## Historical Note

The later live-assembly rewrite moved away from frozen generated fixtures and
assembled upstream test bodies live inside a test-only assembler island. That
later approach fixed important blind spots around memory semantics and became
the current guardrail described in `docs/testing.md`.
