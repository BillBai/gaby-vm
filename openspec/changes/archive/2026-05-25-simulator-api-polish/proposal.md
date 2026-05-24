## Why

Three small `gaby_vm::Simulator` polishes that don't individually merit their
own change, but together harden the public Simulator surface before downstream
code starts depending on it:

1. A nested struct named `MemoryWrite` that reads like a verb ("execute a
   memory write") when its actual role is the payload an observer receives
   when the Simulator records a guest memory write. The sibling alias is
   `MemoryWriteObserver`; the payload type should match the observer-pattern
   naming.
2. A constructor `Simulator(PredecodeCache*, void* stack_buffer, size_t
   stack_size)` that accepts `stack_size = 0` or a few-byte buffer and
   silently writes `(top - 16-byte-align)` into SP. The first guest store
   then SIGSEGVs deep inside a leaf, far from the real bug.
3. `vsim.SetCPUFeatures(vixl::CPUFeatures::All())` in the same constructor —
   too wide for the project's V1 EL0 user-mode scope (`AGENTS.md` «Scope»).
   The auditor will also consult this set when pre-screening cache ranges
   under `RegisterCodeRange`, so the over-broad feature set causes non-user-
   mode encodings to be predecoded for nothing. There is currently no
   record pointing at this as future work.

Now is the right moment. The public `gaby_vm::Simulator` API was introduced
recently by `predecode-cache-core` and has not stabilized, so a rename is
cheap; no external embedder is supplying odd-sized stack buffers yet; and the
CPU-features narrowing gets a documented hook before the cache's auditor
pre-screen starts mattering.

## What Changes

- **BREAKING** Rename the public observer payload
  `gaby_vm::Simulator::MemoryWrite` → `gaby_vm::Simulator::MemoryWriteEvent`.
  The alias `MemoryWriteObserver` keeps its name; only the struct moves.
  In-tree consumers update: `ForwardingWriteSink`, `ShadowRunner`,
  `test/reentrancy_test.cc`. The imported
  `vixl::aarch64::MemoryWriteSink::Record(addr, size, lo, hi)` interface and
  the unrelated `DivergenceReport::Kind::MemoryWrite` enum value are
  untouched.
- Add `gaby_vm::Simulator::kMinStackSize` (12 KiB — see `design.md` for the
  justification against the VIXL `SimStack` reference total) and have the
  constructor `VIXL_CHECK` it, aborting with a diagnostic that names the
  rejected size and the floor.
- Replace the existing one-line comment above
  `vsim.SetCPUFeatures(vixl::CPUFeatures::All())` with a
  `// TODO(simulator-cpu-features):` block recording (a) why `All()` is used
  during V1 bring-up, (b) why a narrow EL0-user-mode subset is the eventual
  target, and (c) the deferred follow-up. No behavior change; comment only.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `aarch64-simulator`: adds one new requirement on `Simulator` constructor
  input validation (the minimum-stack-buffer-size contract). The struct
  rename and the CPU-features TODO are implementation-only and add no spec
  requirements — the existing spec does not mention `MemoryWrite` or the CPU
  feature set the imported simulator is configured with.

## Impact

- **Public API (breaking, but no out-of-tree consumers exist yet):**
  `gaby_vm::Simulator::MemoryWrite` → `gaby_vm::Simulator::MemoryWriteEvent`.
  Touched in-tree: `include/gaby_vm/simulator.h`, `src/gaby_vm/simulator.cc`
  (`ForwardingWriteSink`), `src/gaby_vm/shadow_runner.cc` (eight references),
  `test/reentrancy_test.cc` (one observer lambda).
- **Public API (behavioral):** the `Simulator` constructor now aborts on
  undersized stack buffers instead of silently constructing one that crashes
  on first guest store. Every existing test already allocates 16 KiB
  (`StackBuffer` in `test/typed_register_io_test.cc`, `test/reentrancy_test.cc`,
  `test/shadow_runner_test.cc`, `test/typed_register_io_abort_test.cc`,
  `test/simulator_correctness.cc`) or 1 MiB
  (`test/workload_shadow_test.cc`); no existing test breaks.
- **Files added:** one new abort-style test (death-test for the new minimum
  contract). Likely `test/simulator_constructor_stack_test.cc`, modeled on
  the existing `test/typed_register_io_abort_test.cc`.
- **Imported VIXL tree:** untouched. No new `// gaby-vm` markers.
- **Dependencies:** none. This change ships independently of any planned
  follow-up (including the CPU-features narrowing the TODO points at).
