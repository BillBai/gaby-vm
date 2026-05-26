## Why

`predecode-cache-benchmark` (archived 2026-05-26) measured the cache
engine at ~4× on `bench_baseline` (mixed workload) and ~14.6× on
`bench_smoke`. That is at the low end of the design's "几倍量级"
(several times) soft target and well below the deep-dive's 30×
aspiration. Profiling `ExecuteInstructionCached`
(`src/aarch64/simulator-aarch64.h:1496-1560`) shows two distinct
per-instruction overheads that the cache *should* have amortized away
but didn't:

1. **Leaf dispatch through `std::function`.** `FormToVisitorFnMap` is
   `std::unordered_map<uint32_t, std::function<void(Simulator*, const
   Instruction*)>>` (`simulator-aarch64.h:5628-5630`); each
   `PredecodedEntry::leaf` is a pointer to one of those `std::function`
   slots. Every cached instruction pays a `std::function` call
   indirection on top of the actual leaf — which, in turn, dispatches
   to a `Simulator::Sim*` member function. The intermediate
   `std::function` exists only because the map's value type is type-
   erased; the underlying targets are all pointers-to-member-function
   with the same signature.

2. **BType / guarded-page enforcement on every instruction.** The hot
   path runs `if (PcIsInGuardedPage() && (ReadBType() != DefaultBType))
   { ... }` unconditionally per step. `PcIsInGuardedPage()` is false on
   all current workloads (no test marks pages as guarded), and the
   BType check is meaningful only for a small set of forms (PACIASP,
   PACIBSP, BTI, BRK, HLT, exception-causing). The check's per-step
   cost is paid by every instruction — the cache's whole point is to
   shoulder *exactly* that kind of per-step decode work at predecode
   time instead.

Neither overhead is intrinsic to "predecoded cache lookup + leaf
dispatch". Both leak the imported decoder's per-step bookkeeping into
the cache hot path. Removing them is the next step toward the design's
~30× aspiration and the right framing for the cache's purpose: per-step
work that depends only on the instruction encoding belongs in the
predecode pass, not in the execution loop.

## What Changes

- **Replace `std::function` leaf dispatch with a raw pointer-to-
  member-function.** Change `FormToVisitorFnMap`'s value type from
  `std::function<void(Simulator*, const Instruction*)>` to
  `void (Simulator::*)(const Instruction*)`. Update
  `ResolvePredecodeLeaf` to return the raw pmf as an opaque handle and
  `ExecuteInstructionCached`'s call site to invoke it as a direct
  member-function call. The map's stored values are already pointers-
  to-member-function (`&Simulator::SimulateXyz`), so no value-shape
  conversion is required — only the type-erasure wrapper is removed.

- **Gate the BType / guarded-page check at predecode time.** Each
  `PredecodedEntry` already has a `reserved` 32-bit slot (cache-core
  D8). Use one bit of it to flag whether the entry is BTI-relevant:
  PACIASP, PACIBSP, BTI, BRK, HLT, and exception-causing forms get the
  bit set; everything else does not. `ExecuteInstructionCached` runs
  the BType / guarded-page check only when the bit is set. The
  classification is built once during `RegisterCodeRange`'s predecode
  pass.

- **Correctness regression coverage stays unchanged.** ShadowRunner is
  the V1 oracle (predecode-cache spec); `workload_shadow_test`,
  `simulator_correctness`, and the rest of the dual-path CTest suite
  MUST report zero divergence after the change.

- **Benchmark numbers are recorded but not gated.** `bench_baseline
  --engine cache` and `bench_smoke --engine cache` MUST improve over
  the predecode-cache-benchmark baseline (~4× / ~14.6×), with the new
  numbers captured in the design doc's appendix. No committed `N×`,
  consistent with `gaby-vm-predecode-cache-design.md` §4.5.

### Non-Goals

- The `gaby_vm` public API surface (`include/gaby_vm/`). The
  `PredecodedEntry` struct layout is unchanged; the `reserved` field
  was reserved precisely for this kind of follow-up.
- New tests, new workloads, new public types, or a microbenchmark
  framework.
- A hard performance target.
- `movprfx` form-hash tracking (option (c) from the scoping
  discussion). That tangles with SVE semantics and is best handled in
  its own follow-up after this change lands and ShadowRunner has
  re-validated the simpler pieces.

## Capabilities

### New Capabilities

*(none — this change modifies one existing capability)*

### Modified Capabilities

- `predecode-cache`: adds one new requirement — cache-track per-step
  work that depends only on the instruction encoding MUST be performed
  at most once per instruction, at predecode time, and MUST NOT be
  repeated on each execution of that instruction. This codifies the
  cache's purpose so that future changes cannot silently re-introduce
  per-step decode work into the hot path.

## Impact

- **Depends on `predecode-cache-core` and `predecode-cache-benchmark`**
  (both archived). Consumes their existing internals — `FormToVisitor
  FnMap`, `PredecodedEntry`, the predecode pass — and verifies via the
  archived `bench/` cache-on / cache-off comparison.
- **Files**: `src/aarch64/simulator-aarch64.{h,cc}` (marker-block edits
  to imported VIXL files — see `docs/architecture.md` for the
  convention), `src/gaby_vm/predecode_cache.cc` (predecode pass adds
  the BTI-relevant classification), `docs/refs/gaby-vm-predecode-cache-
  design.md` (appendix records the new measured numbers).
- **Public API**: no surface change. `PredecodedEntry::reserved` is
  documented in `include/gaby_vm/predecode_cache.h` as a reserved
  slot — its internal use does not affect the embedder.
- **VIXL import boundary**: edits stay inside marker blocks
  (`// gaby-vm BEGIN`/`// gaby-vm END`); no upstream VIXL semantics are
  altered. The `FormToVisitorFnMap` value-type change is the most
  invasive — it's a member-name-preserving type swap, not a redesign.
- **ShadowRunner remains the correctness oracle**: any divergence
  introduced by the BTI classification is caught by `workload_shadow`,
  which exercises the full mixed workload.
