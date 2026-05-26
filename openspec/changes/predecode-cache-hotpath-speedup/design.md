## Context

`predecode-cache-core` shipped V1 of the cache: it produces a flat
`PredecodedEntry[]` indexed by `(pc - range_start)/4`, stores
`(form_hash, leaf)` per entry, and dispatches through
`Simulator::ExecuteInstructionCached`. Correctness is validated by
ShadowRunner. The cache exists for *speed*, but predecode-cache-core
intentionally took no measurement — the deferred measurement landed
in `predecode-cache-benchmark` (archived 2026-05-26).

The benchmark numbers (Release, Apple Silicon, single run):

| Workload | Decoder | Cache | Speedup |
|---|---|---|---|
| `smoke`  | 221 k iters/sec | 3.3 M iters/sec | ~14.6× |
| `mixed`  | 68  iters/sec    | 272 iters/sec   | ~4.0×  |

The smoke (branch-free) workload reaches ~34 cycles/instruction; the
mixed workload sits at ~200 cycles/instruction. The deep-dive's 10
cycle/instruction aspiration is still some distance away. Two cache
hot-path overheads sit between us and that aspiration:

- `ExecuteInstructionCached` (`src/aarch64/simulator-aarch64.h:1496-
  1560`) calls the leaf through a `std::function`. The map
  `FormToVisitorFnMap` is
  `std::unordered_map<uint32_t, std::function<void(Simulator*, const
  Instruction*)>>`; every entry is a pointer to one of those map
  slots. So each cached instruction does: array load → form_hash store
  → load `std::function*` → call through `std::function` → call
  through the underlying pmf. The `std::function` is a redundant level
  of indirection — the underlying targets are uniformly
  pointers-to-member-function `&Simulator::SimulateXxx`.
- The same hot path runs `if (PcIsInGuardedPage() && (ReadBType() !=
  DefaultBType)) {...}` on every step. None of the committed workloads
  marks pages as guarded; even on workloads that do, the check is
  meaningful only for a small set of forms. The check's per-step cost
  is paid by every instruction, including ALU forms that have nothing
  to do with BType.

Neither overhead is intrinsic. The cache's *purpose* is to move
encoding-dependent per-step work to predecode time. Both overheads
violate that purpose.

The fix lives entirely inside the gaby-vm marker blocks of the
imported VIXL simulator file and inside the predecode pass — no public
API change, no new test surface, no new workload.

## Goals / Non-Goals

**Goals:**

- Bring cache-track per-instruction non-leaf overhead down to its
  minimum: predecoded entry fetch, leaf handle fetch, direct call.
- Keep predecode-cache's correctness invariants exactly as they are:
  ShadowRunner divergence-free on `workload_shadow` (mixed + smoke)
  and on `simulator_correctness`'s dual-path families.
- Codify the optimization invariant in the `predecode-cache`
  capability so that a future regression cannot silently re-introduce
  per-step decode work.

**Non-Goals:**

- A committed `N×` target. Acceptance stays soft (a meaningful,
  measured improvement over the predecode-cache-benchmark baseline).
- `movprfx` form-hash tracking (proposal scope option (c)) — defers
  to a follow-up; SVE / movprfx interaction wants its own change.
- Public API edits, new tests, new workloads, microbenchmarks.
- Any restructuring of `FormToVisitorFnMap`'s contents — only its
  value type changes.

## Decisions

### D1. Replace `std::function` leaf with a raw pointer-to-member-function

**Decision.** Change `FormToVisitorFnMap`'s value type from
`std::function<void(Simulator*, const Instruction*)>` to
`void (Simulator::*)(const Instruction*)`. `ResolvePredecodeLeaf`
keeps its `const void*` return type — but the underlying pointee is
now a pmf, not a `std::function`. `ExecuteInstructionCached`'s call
site casts back to the pmf type and invokes via
`(this->*pmf)(pc_)`.

**Why.** The map's stored values are already exclusively
`&Simulator::SimulateXxx` member-function-pointers — `std::function`
adds nothing for them except a per-call indirection and a capture-
state read. A pmf call lowers to a tighter sequence on Clang/AppleClang
(one load + one indirect call). The map's structure (form_hash
→ leaf), the predecode pass's logic, and the cache entry layout all
stay exactly as they are. `PredecodedEntry::leaf`'s type
(`const void*`) is unchanged — the opacity is what lets us swap the
underlying type without touching the public header.

**Alternatives.** (a) Keep `std::function` but mark `[[gnu::always_
inline]]` etc. — rejected: `std::function`'s indirection isn't
inlineable across the call. (b) Switch every leaf to a free function
taking `Simulator*` — rejected: the leaves are all class members for a
reason (they touch private state); the change would ripple through the
entire VIXL leaf surface. (c) Build a flat `[]` array of leaves
indexed by form-hash — rejected: form-hashes are sparse 32-bit values;
a flat array is wasteful and orthogonal to this change.

### D2. Predecode-time BTI classification, encoded in `PredecodedEntry::reserved`

**Decision.** Each `PredecodedEntry` carries a 32-bit `reserved` slot
(declared by predecode-cache-core D8, currently always zero). Use bit
0 as a `bti_relevant` flag. The predecode pass classifies an
instruction as BTI-relevant iff it is PACIASP, PACIBSP, BTI, BRK, HLT,
or an exception-causing form (SVC/HVC/SMC/UDF/...). The runtime check
becomes:

```cpp
if (entry->reserved & 1) {
  // existing PcIsInGuardedPage / ReadBType / IsBti / IsPAuth /
  // IsException dispatch
}
```

Everything else gets the check elided.

**Why.** The check is meaningful only for instructions that interact
with BType. The set of such instructions is fixed and small — the
predecode pass already iterates every instruction to compute
`form_hash`, so the classification is free at predecode time.
Encoding it in the `reserved` slot rather than adding a new field
keeps `sizeof(PredecodedEntry) == 16` and respects the existing
public-header `static_assert` (`predecode_cache.h:108-109`).

**Alternatives.** (a) Add a parallel `bool[]` array of classification
bits — rejected: doubles cache-line pressure and complicates the
flat-array indexing the design carefully preserved. (b) Run the check
on every instruction and hope the branch predictor handles it —
rejected: that's what we have today and it's measurably costly. (c)
Decode the BTI relevance from the form_hash at runtime — rejected:
form_hash is opaque to non-VIXL code; we'd be re-introducing a per-
step decode pass exactly inverse to the cache's purpose.

### D3. Classification runs inside the existing predecode pass

**Decision.** `predecode_cache.cc`'s populate loop already calls
`Simulator::ResolvePredecodeLeaf(form_hash)` for each instruction.
That same loop classifies BTI relevance — a single switch over the
form-hash for the small relevant set, or a query through a static
table. The classification writes bit 0 of `PredecodedEntry::reserved`.

**Why.** No new pass, no new public type. The predecode pass is
already O(n) over the registered range; the classification adds a
constant per-instruction cost there, which is paid once and amortized
forever after. Keeping it in one pass also means there is no second
place a future change could forget to update.

**Alternatives.** (a) A second pass over the entries — rejected: two
passes for one O(n) job. (b) Compute the classification lazily on
first hot-path execution — rejected: lazy bookkeeping in the hot path
is exactly what we're trying to remove.

### D4. `ResolvePredecodeLeaf` keeps its `const void*` opaque-handle contract

**Decision.** The signature stays `static const void* Simulator::
ResolvePredecodeLeaf(uint32_t form_hash)`. The returned pointer is now
a pmf-typed pointer rather than a `std::function*`, but
`predecode_cache.cc` doesn't care — it stores it verbatim in
`PredecodedEntry::leaf`. The cast back at the call site changes:

```cpp
// Before
(*static_cast<const FormToVisitorFnMap::mapped_type*>(entry->leaf))
    (this, pc_);
// After
const auto pmf =
    *static_cast<const FormToVisitorFnMap::mapped_type*>(entry->leaf);
(this->*pmf)(pc_);
```

**Why.** Keeping `const void*` on the boundary preserves the layer
that lets `predecode_cache.cc` avoid pulling VIXL types — the rule
established by `gaby-vm-predecode-cache-design.md` §5.1 and codified
by `predecode-cache-core`. The pmf is opaque to the predecode pass;
only `ExecuteInstructionCached` interprets it.

### D5. Acceptance is the same dual-pass + benchmark posture as core

**Decision.** Acceptance for this change is:

1. `workload_shadow_test` reports zero divergence on `mixed` and
   `smoke`.
2. `simulator_correctness` (every hand-encoded family) passes on both
   tracks.
3. `bench_baseline --engine cache` and `bench_smoke --engine cache`
   improve measurably over the predecode-cache-benchmark baseline
   (`~4×` / `~14.6×` cache-vs-decoder). The actual deltas are
   recorded in `docs/refs/gaby-vm-predecode-cache-design.md`'s
   appendix; no committed `N×`.

**Why.** Mirrors predecode-cache-core's acceptance: hard correctness +
soft performance. The benchmark harness from predecode-cache-benchmark
is the measurement instrument.

## Risks / Trade-offs

- **A miss in the BTI-relevant classification is silently wrong on
  guarded-page workloads.** → ShadowRunner catches it; even today the
  committed workloads do not exercise guarded pages, so we add a
  hand-encoded sub-test under `simulator_correctness`'s control-flow
  family that includes a PACI[AB]SP + BTI sequence. The dual-track
  comparison then proves the classification matches the architectural
  behavior.

- **Pmf invocation through an opaque handle is fragile across compilers
  / standard libraries.** → Pointer-to-member-function size and
  representation are implementation-defined but stable on Itanium-ABI
  toolchains (Clang/GCC on Apple/Linux). The `static_cast<const T*>`
  + dereference round-trip is well-defined: `T` is exactly the
  `mapped_type` of the map the pointee came from. A `static_assert`
  on `sizeof(FormToVisitorFnMap::mapped_type)` in
  `simulator-aarch64.cc` guards against silent ABI drift on a future
  toolchain change.

- **The cache-line pressure of `PredecodedEntry::reserved` is 4 bytes
  we now read on every step.** → Already in the same cache line as
  `form_hash` (entry is 16 bytes, aligned, both fields in the first
  8 bytes of the entry). No new cache-line cost.

- **The `std::function` removal might break a callsite outside
  `FormToVisitorFnMap` that we missed.** → A grep before commit
  confirms `FormToVisitorFnMap::mapped_type` is referenced only in
  `simulator-aarch64.cc` (map initialization), `simulator-aarch64.h`
  (`ResolvePredecodeLeaf`, `ExecuteInstructionCached`), and nowhere
  else.

- **This change cannot land before predecode-cache-benchmark is
  archived.** → Already archived (commit `0e8ce02`). No remaining
  ordering dependency.

## Migration Plan

The change is binary-and-source compatible for embedders:

- `gaby_vm::PredecodeCache` public API: unchanged.
- `PredecodedEntry::reserved` field: documented behavior expands from
  "zero" to "internally allocated", per the existing reserved-slot
  contract in `include/gaby_vm/predecode_cache.h`.
- Behavior: identical, per ShadowRunner.

Rollback is a `git revert` of the change's commits. The change touches
only `src/` and the `docs/refs/` design doc appendix; rolling back
restores the previous cache hot path with no data-format migration.

## Open Questions

*(none — D1–D5 settle the implementation. The benchmark numbers
expected post-change will be measured during implementation and
recorded in the design-doc appendix.)*
