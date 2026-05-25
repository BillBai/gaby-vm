## Why

Gaby-VM's stated primary optimization is `predecode once → cache decoded
dispatch target → execute cached path repeatedly` (`AGENTS.md`). None of it
exists yet. Every guest instruction currently pays the full imported VIXL
dispatch chain — a heap-allocated `Metadata`, two string hashes, a visitor-list
walk, and an `unordered_map` lookup — on the order of 300–500 cycles, of which
the leaf that does the real work is roughly 5%. The cycle breakdown and the
twelve risks the design must answer (R1–R12) are in
[`docs/refs/vixl-fetch-decode-dispatch-deep-dive.md`](../../../docs/refs/vixl-fetch-decode-dispatch-deep-dive.md);
the settled design is
[`docs/refs/gaby-vm-predecode-cache-design.md`](../../../docs/refs/gaby-vm-predecode-cache-design.md).

The spec blocker is already cleared: `relax-vixl-import-boundary` (archived
2026-05-20) relaxed the `aarch64-simulator` *preserved structurally*
requirement so an alternate dispatch path may exist alongside the imported
flow. This change builds that path.

## What Changes

- **New `predecode-cache` capability.** A `gaby_vm::PredecodeCache` — a
  standalone object, shared across `Simulator` instances — runs a one-time
  predecode pass over each registered code range and maps every 4-byte
  instruction word to a 16-byte `PredecodedEntry` holding the instruction's
  form hash and leaf function pointer. The hot path collapses to "one array
  load + one indirect call"; leaf semantics are untouched.
- **New `gaby_vm::Simulator` public API (Pimpl), dual-track.** `RunFrom` takes
  the cache fast path; `DebugRunFrom` takes the imported `Decoder → visitor →
  leaf` flow with full trace/debug observability. `StepOnce` / `DebugStepOnce`
  expose lockstep single-stepping. There is **no runtime switch** between
  tracks — splitting at the API layer is the design's answer to
  decoder-state-coherence hazards (`form_hash_`, `last_instr_`, MOVPRFX
  chains). Execution calls are **re-entrant** on one `Simulator` — a native
  bridge callback may nest a `RunFrom` — by saving and restoring the
  interpreter cursor (see `design.md` D10).
- **New `gaby_vm::testing::ShadowRunner`.** The V1 correctness oracle: runs the
  cache path and the decoder path in lockstep over the same code and reports
  the first per-instruction divergence (general/FP registers, PC, flags, and
  memory writes).
- **First `// gaby-vm` markers in the imported tree.** Marker-bracketed
  additions to `src/aarch64/simulator-aarch64.{h,cc}`: a `MemoryWriteSink`
  hook (for ShadowRunner), `ExecuteInstructionCached`, and `StepOnce` /
  `DebugStepOnce`. Each marker reason names the design doc, as the relaxed
  `aarch64-simulator` spec now requires.
- **`RegisterCodeRange` is validating and append-only.** Overlapping ranges,
  unsupported CPU features (pre-screened by `CPUFeaturesAuditor` at populate
  time), bad sizes, and allocation failure are reported via a C-ABI-friendly
  `RegisterStatus` plus a queryable `ErrorDetail`. A `RunFrom` PC outside every
  registered range aborts rather than silently falling back.
- **Correctness-test rework.** `test/simulator_correctness.cc` becomes a
  dual-path harness — every hand-encoded sequence runs through both `RunFrom`
  and `DebugRunFrom` and the post-run state is asserted equal. A new
  `test/shadow_runner_test.cc` covers the oracle, including an injected-bug
  case that proves it actually catches divergence.

### Non-Goals

- **Benchmark measurement of the cache.** Running the workloads cache-on and
  comparing throughput against the decoder baseline is the immediate
  follow-up change, `predecode-cache-benchmark` (rationale in `design.md`
  D5). This change has no performance acceptance criterion of its own — its
  acceptance is **hard correctness**: dual-path tests green + zero
  ShadowRunner divergence.
- **V2 optimizations**, per `gaby-vm-predecode-cache-design.md` §8: operand
  pre-extraction, basic-block linking, direct threading, the 8-byte per-form
  thunk entry (V1 uses a simpler 16-byte direct entry — see `design.md` D8),
  `FlushCodeRange` / cache invalidation, SMC support, SVE Z/P/FFR shadow
  comparison, a memory cap or LRU on the cache, and any hard `N×` number.

## Capabilities

### New Capabilities

- `predecode-cache`: the predecode/dispatch cache subsystem — `PredecodeCache`
  lifecycle and `RegisterCodeRange`, the `PredecodedEntry` layout, PC→entry
  lookup, the dual-track `gaby_vm::Simulator` API, the
  `MemoryWriteSink` hook on imported `Simulator`, the `ShadowRunner` oracle,
  and the CTest coverage that proves cache/decoder equivalence. Owns
  `include/gaby_vm/{predecode_cache,simulator,shadow_runner}.h` and the new
  `src/gaby_vm/` tree.

### Modified Capabilities

- `aarch64-simulator`: the two `simulator_correctness` requirements are
  **removed** — they are superseded by the dual-path correctness harness
  specified by the new `predecode-cache` capability. `test/simulator_correctness.cc`
  is reworked to drive the `gaby_vm::Simulator` public API rather than calling
  `vixl::aarch64::Simulator::RunFrom` directly; the imported `Decoder → visitor
  → leaf` flow it verified stays exercised end-to-end as the `DebugRunFrom`
  leg. No baseline-family coverage is lost — it relocates to `predecode-cache`.
  The marker convention, tier-import, license, namespace, and structural
  requirements are unchanged.

*(`benchmark-harness` is modified by the follow-up `predecode-cache-benchmark`
change, not this one — see `design.md` D5.)*

## Impact

- **New source**: `include/gaby_vm/{predecode_cache,simulator,shadow_runner}.h`
  and a new `src/gaby_vm/` directory (`predecode_cache.cc`, `simulator.cc`,
  `shadow_runner.cc`).
- **Imported files**: `src/aarch64/simulator-aarch64.{h,cc}` gain their first
  marker regions; `git grep gaby-vm src/` must continue to enumerate every
  drift.
- **Tests**: `test/simulator_correctness.cc` reworked; `test/shadow_runner_test.cc`
  added; `test/CMakeLists.txt` updated.
- **Build**: the `gaby_vm` library compiles the new `src/gaby_vm/` sources;
  public headers keep `vixl::*` types unexposed (existing public-header rule).
- **Memory**: with V1's 16-byte direct entry, a registered range costs 4× its
  code size in cache entries (4-byte instruction → 16-byte entry). For the
  expected 1–10 MB patch sizes that is 4–40 MB of cache. The embedder budgets
  cache memory; V1 adds no cap. (V2's 8-byte thunk entry would halve this.)
- **Threading**: the cache is shared across `Simulator` instances.
  Registration and range-table search are guarded by a single-writer /
  multiple-reader lock (`RegisterCodeRange` is the writer); the common
  per-instruction fast path — a hit in the `Simulator`-local `cur_range_` —
  stays lock-free. Consistent with the one-`Simulator`-per-thread model in
  [`docs/architecture.md`](../../../docs/architecture.md).
