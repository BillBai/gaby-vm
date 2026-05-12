## Why

The existing `simulator_smoke` test only proves that a `vixl::aarch64::Simulator`
can be constructed and run a single NOP without crashing — it asserts nothing
about *what the simulator computed*. With Tiers 1–3 of the VIXL simulator just
imported (per the `extract-vixl-aarch64-simulator-baseline` change), we have no
test that would catch a silent regression in the imported leaf semantics. Every
future change — upstream re-syncs, refactors, the eventual predecode/dispatch
cache — needs a regression net before it lands. This change builds that net.

## What Changes

- Add a new CTest-registered binary `simulator_correctness_test` (alongside
  `gaby_vm_smoke` and `simulator_smoke`) that:
  - Loads small instruction sequences, encoded as raw `uint32_t` arrays, into
    Simulator-addressable memory.
  - Drives them through `vixl::aarch64::Simulator::RunFrom`.
  - Asserts the resulting register and memory state matches expectation.
- Cover at minimum the following AArch64 instruction families:
  - Integer arithmetic — `ADD`, `SUB`, `MUL`.
  - Logical — `AND`, `ORR`, `EOR`.
  - Load/store — `LDR`, `STR` with both immediate and register offsets.
  - Conditional control flow — `B.cond`, `CBZ`, `CBNZ`.
  - Procedure call / return — `BL`, `RET`.
- Reuse `simulator_smoke`'s privileged build pattern verbatim:
  - `target_include_directories(... PRIVATE ${PROJECT_SOURCE_DIR}/src)`.
  - Compile definitions `VIXL_INCLUDE_TARGET_A64`,
    `VIXL_INCLUDE_SIMULATOR_AARCH64`, and `$<$<CONFIG:Debug>:VIXL_DEBUG>` set
    `PRIVATE`.
  - No change to `include/gaby_vm/` or any other public surface.
- Test SHALL pass cleanly under both `dev-debug` and `dev-release` presets.

### Non-Goals (deferred to later changes)

- Importing VIXL's own `test/` directory. It is large and depends on the
  assembler / macro-assembler, which are Tier 0 and stay out per the
  import-baseline change's scope.
- Cross-checking simulator output against native AArch64 hardware execution.
- Performance or throughput measurement — handled by a separate
  `baseline-benchmark-harness` change.
- SIMD, SVE, floating-point, or pointer-authentication coverage. These follow
  once integer correctness is solid.
- Any change to imported `src/` files; this change adds tests only.

## Capabilities

### New Capabilities

*(none — this change extends an existing capability rather than introducing one)*

### Modified Capabilities

- `aarch64-simulator`: extend the test-contract section of the spec. The
  existing requirements that pin "simulator constructs and executes a single
  NOP" and "`gaby_vm_smoke` continues to pass" remain intact; a new requirement
  is added stating that the simulator must produce architecturally-correct
  register and memory state for hand-encoded sequences spanning the integer,
  logical, load/store, conditional-branch, and call/return instruction families
  listed above. The change is purely additive — no existing requirement is
  weakened or removed.

## Impact

- **New source file:** `test/simulator_correctness_test.cc` — one self-contained
  translation unit holding the encoded sequences, a small harness that copies
  them into Simulator memory and calls `RunFrom`, and the post-run assertions.
- **Build wiring:** `test/CMakeLists.txt` gains a third `add_executable` +
  `add_test` block, mirroring the `simulator_smoke` block (PRIVATE include of
  `${PROJECT_SOURCE_DIR}/src`, the same three VIXL compile definitions,
  `gaby_vm_apply_compile_flags`). New CTest case name: `simulator_correctness`.
- **No changes to** imported `src/` files, public headers under
  `include/gaby_vm/`, exported CMake targets, or external dependencies.
- **Spec delta:** `openspec/specs/aarch64-simulator/spec.md` grows by one new
  requirement (with scenarios) describing the broadened test contract.
- **CI / future work:** any change that perturbs imported integer leaf
  semantics — re-syncs, refactors, or the predecode/dispatch cache — will fail
  this test before merge, which is exactly the regression net the project's
  validation strategy in `CLAUDE.md` calls for.
