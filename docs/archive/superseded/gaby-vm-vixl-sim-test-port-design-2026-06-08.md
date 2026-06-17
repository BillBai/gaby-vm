# Porting VIXL Simulator Tests As A Guardrail - Design - 2026-06-08

> **What this is:** an approved design draft used to drive the implementation
> plan for a broad VIXL AArch64 simulator test port. The goal was to establish a
> correctness guardrail before more aggressive dispatch and operand-predecode
> optimization work.
>
> **What this is not:** not an OpenSpec change and not a schedule. Normative
> testing requirements should move into
> [`../../openspec/specs/aarch64-simulator/spec.md`](../../../openspec/specs/aarch64-simulator/spec.md)
> only when they are intended to become current requirements.
>
> **Status:** approved on 2026-06-08, then superseded by the later live-assembly
> rewrite.

## 1. Why This Was Needed

The planned optimizations touched the execution hot path shared by all
instructions: dispatch contraction, operand predecode, and eventually threaded
dispatch. Those changes must preserve VIXL leaf semantics exactly.

The existing regression net in `test/simulator_correctness.cc` only covered a
small set of instruction families: basic arithmetic, logical operations, simple
loads/stores, branches, `BL`, and `RET`. That was too sparse to catch many
possible regressions in less common instruction shapes.

The design goal was to port enough upstream VIXL execution tests to make the
cache/decoder split a real guardrail before changing the hot path further.

## 2. Decisions

| Topic | Decision | Reason |
| --- | --- | --- |
| Coverage | Broad semantic port | Port execution-style cases from `test-assembler-aarch64`, `test-assembler-fp-aarch64`, and `test-assembler-neon-aarch64`. This gives broad integer, logical, memory, branch, FP, and NEON coverage. |
| Byte extraction | Authorship-time extraction tool | A developer-only tool links the real `../vixl` MacroAssembler and Simulator, emits code bytes plus expected state, and commits generated fixtures. Build and runtime do not depend on `../vixl`. |
| Oracle | Differential plus absolute | The main check is cache track vs decoder track. VIXL-harvested assertions provide an absolute anchor so both tracks cannot drift together silently. |

The extraction tool was considered compatible with the Tier-0 import boundary
because it was authorship-time only. Its generated C++ data would be committed,
like hand-copied `llvm-mc` encodings.

## 3. Architecture: Harvest During Development, Replay In Shipping Tests

```text
Development time, requires ../vixl              Shipping/CI, self-contained
tools/vixl_test_extract/                        test/vixl_port/
  capture macros                                  generated/*.inc fixtures
  include real VIXL tests                         replay harness
  link real VIXL MASM + Simulator                 vixl_port_{int,fp,neon}
  write fixtures and filter report                CTest executables
```

Generated fixture headers are committed. Normal builds, CI, and iOS builds do
not need `../vixl` or a runtime assembler. Developers only enable the extraction
path when refreshing or adding cases.

## 4. Extraction Engine

The planned extraction path relied on VIXL's test macro structure:

- Define `VIXL_INCLUDE_SIMULATOR_AARCH64`.
- Preempt the VIXL test header include guard with a capture header.
- Provide replacement `TEST`, `SETUP`, `START`, `END`, `RUN`, and
  `ASSERT_EQUAL_*` macros.
- Include VIXL's test `.cc` files under those macros.

Each `TEST` body would then assemble with the real VIXL `MacroAssembler`, run on
the real VIXL `Simulator`, and record the result.

Two routes were allowed:

- A1: swallow whole files through macro redefinition.
- A3: manually copy exceptional test bodies into the extraction tool when macro
  capture could not handle them.

For each case, the fixture was expected to record:

- Test name and source family.
- Required and seen CPU features.
- Emitted instruction words.
- Entry architectural state: GP registers, SP, PC, vector registers, NZCV,
  FPCR, and FPSR.
- Harvested `ASSERT_EQUAL_*` targets.

The Phase 0 plan was to prove the full pipeline on roughly ten integer cases
before expanding coverage.

## 5. Fixture Format

Fixtures were planned as generated C++ `constexpr` data:

- `uint32_t[]` instruction words.
- A `RegisterFile` entry state.
- An array of assertion targets.

No runtime file IO and no JSON parser would be introduced. This fit the
project's rule that predecoded data is ordinary data and tests should remain
self-contained.

## 6. Replay Harness And Oracle Contract

Each fixture would run from the same entry state on two tracks:

- Cache track: `RunFrom`.
- Decoder track: `DebugRunFrom`.

The replay harness would then apply:

- Differential oracle: the whole register file must match between tracks.
- Absolute oracle: both tracks must satisfy every harvested VIXL assertion
  target.

Address relocation was handled by separating the two checks:

- Differential comparison is immune to relocation because both gaby-vm tracks
  run the same bytes at the same replay address.
- Absolute checks only use VIXL assertions that are portable across addresses.
  Assertions whose expected value is a code or stack address are skipped and
  reported, with the differential oracle still covering them.

Each body includes the VIXL `START` / `END` prologue and epilogue and normally
terminates with `RET`. The gaby-vm null-LR contract stops execution when `RET`
returns to zero. The replay stack uses `test/embedding_stack.h`.

## 7. Feature Gating And Filtering

The extraction tool filters cases that gaby-vm cannot run:

- Unsupported CPU features.
- Known `VisitUnimplemented` forms, such as MTE, TME, BFloat16,
  unprivileged EL1 load/store forms, and `WFET` / `WFIT`.
- EL1+ behavior, system-register side effects, or exception-model behavior
  outside the V1 EL0 user-mode scope.

Skipped cases must be listed explicitly in the generated report with a reason.
No silent truncation.

## 8. Build Wiring

The extraction tool would be a separate CMake target behind
`-DGABY_VM_BUILD_VIXL_EXTRACT=ON` and `VIXL_SRC_DIR=../vixl`. Only that target
links Tier-0 VIXL.

The shipping replay tests were planned as `vixl_port_integer`,
`vixl_port_fp`, and `vixl_port_neon`, each registered with CTest and consuming
headers from `test/vixl_port/generated/`. They should have no Tier-0 or
`../vixl` dependency.

## 9. Phases

- Phase 0: extraction engine, fixture format, and replay harness on a small
  integer slice.
- Phase 1: integer, logical, memory, branch, and EL0-runnable system cases from
  `test-assembler-aarch64.cc`.
- Phase 2: FP cases from `test-assembler-fp-aarch64.cc`.
- Phase 3: NEON cases from `test-assembler-neon-aarch64.cc`.

Each phase would run extraction, filter unsupported cases, generate fixtures,
turn the replay suite green, and commit fixtures plus manifest.

## 10. Non-Goals

- No SVE or SVE2 tests in this phase.
- No `test-simulator-aarch64.cc` golden-trace matrix in this phase.
- No runtime assembler.
- No runtime data-file format.
- No changes to VIXL leaf semantics.
- Do not "fix" expected values in fixtures to mask real semantic differences.

## 11. Risks And Open Questions

- Macro-capturing whole VIXL files is brittle. Mitigation: prove a small slice
  first, use the A3 fallback, and assert macro assumptions in comments and code.
- The harvested entry state must exactly match the state seen by gaby-vm
  `RunFrom`, or absolute checks will be noisy.
- Six hundred or more fixtures may affect compile time. Split by family and
  source file, then revisit the harness only if compile time becomes a problem.
- Whether EL0-readable system registers such as `CTR_EL0` and `DCZID_EL0`
  should be included.
- Whether to retain a few readable hand-written smoke cases alongside generated
  fixtures.

## 12. Acceptance Criteria

1. With extraction enabled and `../vixl` available, the tool generates fixtures
   and an included/skipped report.
2. With default build settings and no `../vixl`, the three replay tests pass.
3. Every included case passes both differential and absolute oracles, with
   address-dependent absolute checks excluded and reported.
4. A deliberate cache-track defect makes the suite fail.
5. After Phases 1-3, the covered instruction families are significantly broader
   than `test/simulator_correctness.cc`, and the include/skip list is
   quantifiable.
