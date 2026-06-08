# Testing strategy

This is the orientation doc. Detailed, normative requirements for each test
target live in
[`../openspec/specs/aarch64-simulator/spec.md`](../openspec/specs/aarch64-simulator/spec.md);
this document explains the shape so you know *which* spec section to consult.

## Current shape

Tests are stand-alone executables under [`../test/`](../test/), each built
from a single project-authored translation unit, linked against
`gaby_vm::gaby_vm`, and registered with CTest via `add_test`.

The project does not currently use an external testing framework like
GoogleTest or Catch2 — mostly because nobody has needed to pull one in yet.
Hand-rolling the few existing tests has been simpler than integrating a
framework. This is not a long-term position; if the test set grows or starts
needing things like parameterised fixtures, picking up a framework is fine.

When a test needs to reach `vixl::aarch64` types directly (e.g. construct a
`Simulator`), it uses the
[privileged build pattern](build.md#privileged-test-build-pattern):
`PRIVATE` include of `${PROJECT_SOURCE_DIR}/src` plus the same `VIXL_*`
compile definitions that gate the imported sources. Tests that only exercise
the public API don't need that privilege.

## Current targets

- `gaby_vm_smoke` — public-API sanity check. Doesn't reach into imported
  headers.
- `simulator_smoke` — constructs a `vixl::aarch64::Simulator`, decodes and
  executes a single NOP (`0xd503201f`) through the imported
  `Decoder → VisitNamedInstruction → leaf` flow, exits 0.
- `simulator_correctness` — baseline correctness regression. Drives
  hand-encoded `uint32_t` instruction sequences through
  `Simulator::RunFrom` and checks post-run register / memory state. Today it
  covers integer arithmetic (`ADD`, `SUB`, `MUL`), logical (`AND`, `ORR`,
  `EOR`), load/store with both immediate-offset and register-offset modes,
  conditional control flow (`B.cond` after `SUBS`/`CMP`, `CBZ`, `CBNZ`), and
  procedure call / return (`BL` + `RET`). Each sequence terminates via a
  trailing `RET` to a NULL link register, relying on the
  `LR == kEndOfSimAddress` invariant established by
  `Simulator::ResetRegisters`.
- `vixl_port_integer` / `vixl_port_fp` / `vixl_port_neon` — the broad
  correctness guard rail, machine-ported from VIXL's own
  `test-assembler-{aarch64,fp-aarch64,neon-aarch64}.cc`. Far wider coverage
  than `simulator_correctness` (hundreds of instruction forms). See
  [Ported VIXL tests](#ported-vixl-tests-vixl_port) below.

## Ported VIXL tests (`vixl_port`)

`test/vixl_port/` holds a large correctness guard rail ported from VIXL's own
execution test suite, built specifically to catch regressions in the shared
execution hot path before the dispatch / operand-predecode performance work.
Design and plan:
[`refs/gaby-vm-vixl-sim-test-port-design-2026-06-08.md`](refs/gaby-vm-vixl-sim-test-port-design-2026-06-08.md)
and the sibling `-plan-`.

It is split into two halves:

- **Authorship-time extraction tool** — `tools/vixl_test_extract/`. Links the
  reference VIXL (`../vixl`) MacroAssembler + Simulator and, via macro
  redefinition, captures each upstream `TEST()` body into a fixture: the body
  instruction words, the entry register state, and the `ASSERT_EQUAL_*`
  targets. Built **only** behind `-DGABY_VM_BUILD_VIXL_EXTRACT=ON`
  `-DVIXL_SRC_DIR=…`; it is never part of the shipping build (no Tier-0 in
  CI/iOS). Each captured body is self-verified against the *real* VIXL
  simulator before export, so a wrong expectation can never be committed.
- **Shipping replay harness** — `test/vixl_port/vixl_port_runner.*` plus the
  three CTest mains. Consumes only the committed `generated/*.inc` fixtures and
  the gaby_vm public API. Each fixture is replayed on **both** tracks (cache via
  `RunFrom`, decoder via `DebugRunFrom`) under two oracles: a **differential**
  oracle (the two tracks' full `RegisterFile` must match — this is what catches
  a cache-track regression) and an **absolute** oracle (every harvested
  `ASSERT_EQUAL_*` target must hold). The committed fixtures make `ctest`
  completely self-contained: no `../vixl`, no assembler, no extraction tool.

### Refreshing the fixtures

The `generated/*.inc` files and their `manifest_<family>.md` reports are
committed. Regenerate them (e.g. after a VIXL upgrade, or to widen coverage)
with, per family:

```sh
cmake --preset dev-debug -DGABY_VM_BUILD_VIXL_EXTRACT=ON \
  -DVIXL_SRC_DIR=$PWD/../vixl \
  -DVIXL_EXTRACT_TEST_CC=$PWD/../vixl/test/aarch64/test-assembler-aarch64.cc
cmake --build build/debug --target vixl_test_extract
./build/debug/tools/vixl_test_extract/vixl_test_extract \
  test/vixl_port/generated/integer_fixtures.inc Integer
```

Repeat with `test-assembler-fp-aarch64.cc` → `fp_fixtures.inc Fp` and
`test-assembler-neon-aarch64.cc` → `neon_fixtures.inc Neon`.

### What gets dropped, and why (the manifest)

Many upstream tests are not portable to a relocated, different-process replay
and are excluded — but never silently: `manifest_<family>.md` lists every
skipped test with a reason. The filters are structural (robust to opcode detail
and VIXL upgrades):

- **load/store, ADR/ADRP, register-indirect branch, SYS/SYSL** — bake host
  memory/addresses or branch to a host-derived target, which would fault or
  diverge on gaby's load address (`LDR`-literal is exempted: the literal pool is
  forced inline so it travels PC-relative with the body).
- **feature-deny** — MTE / PAuth / GCS / HBC / BF16 / TME / WFXT (gaby cannot
  execute them, or their results are key/modifier/host-address dependent).
- **name-quarantine** — simulator-runtime / system bodies (printf, runtime
  calls, branch interception, MOPS, …).
- **multi-RUN** — tests that drive several SETUP/RUN cycles via a helper called
  in a loop do not fit the single-case fixture model.
- **oversized / non-terminating bodies** — far-branch padding and large guest
  loops are capped (a poor guard-rail fixture and slow to replay).

## Encoding policy

Tests don't assemble at runtime, and the tree doesn't currently ship an
assembler or instruction-emit helper (Tier 0 of the VIXL import is excluded).
Instruction sequences in `simulator_correctness` are raw `uint32_t` arrays.
When a new encoding is needed at authorship time, an external tool such as
`llvm-mc` is a reasonable way to produce the hex, which then gets hand-copied
into the test source. This keeps the build and runtime free of the assembler
machinery without preventing us from leaning on external tools while
authoring tests.

## What's expected from a new test

- Add an `add_executable` + `add_test` pair to
  [`../test/CMakeLists.txt`](../test/CMakeLists.txt).
- Apply `gaby_vm_apply_compile_flags` (the project-policy helper). The
  imported-source relaxation helper is generally not appropriate for tests.
- If the test needs imported headers, use the privileged build pattern.
- The test should pass under the `dev-debug` preset; release builds are
  exercised via `ctest --test-dir build/release` until a release test preset
  is added.
- On failure, exit non-zero with stdout that names the failing sub-case and
  shows actual vs. expected values.
