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
