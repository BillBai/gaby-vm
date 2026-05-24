## Context

The authoritative design for the predecode cache is
[`docs/refs/gaby-vm-predecode-cache-design.md`](../../../docs/refs/gaby-vm-predecode-cache-design.md)
(hereafter **the design doc**). It lands the cache section of
`gaby-vm-modification-sketch.md` and answers the R1–R12 risk list from
`vixl-fetch-decode-dispatch-deep-dive.md`. The design doc is the source of
truth for cache shape, API shape, file/line anchors, and risk handling.

**This artifact does not restate the design doc.** It records the decisions
specific to landing that design as an OpenSpec change: how the work decomposes
into capabilities, which existing capabilities are touched and why, and the
build/test wiring the design doc leaves to implementation.

Current state:

- The imported VIXL simulator (`aarch64-simulator`) executes purely through
  `Decoder → VisitNamedInstruction → leaf`. `relax-vixl-import-boundary`
  (archived 2026-05-20) already relaxed the structural-freeze clause, so an
  alternate dispatch path is permitted alongside the imported flow.
- `include/gaby_vm/` holds only a thin facade (`gaby_vm.h`, `version.h`).
  There is no `src/gaby_vm/` tree, and no `// gaby-vm` marker exists anywhere
  under `src/` — the imported tree is a clean baseline.
- `test/simulator_correctness.cc` drives hand-encoded sequences through
  `vixl::aarch64::Simulator::RunFrom`; `bench/` drives the same simulator type.

## Goals / Non-Goals

**Goals:**

- Ship the V1 cache: a working predecode cache, the dual-track
  `gaby_vm::Simulator`, and the `ShadowRunner` oracle.
- Prove the cache **correct** — dual-path tests green and zero ShadowRunner
  divergence. Speed measurement is the follow-up `predecode-cache-benchmark`
  change (D5); this change has no performance acceptance criterion of its own.
- Keep the imported tree auditable: the first markers this change lands set
  the standard every later imported-file edit will copy.

**Non-Goals:**

- Benchmark measurement of the cache — split into the follow-up
  `predecode-cache-benchmark` change (D5).
- Everything in design doc §8: operand pre-extraction, basic-block linking,
  direct threading, `FlushCodeRange` / invalidation, SMC support, SVE Z/P/FFR
  shadow comparison, a cache memory cap or LRU, and any hard performance
  number.
- The 8-byte per-form thunk entry. V1 ships the simpler 16-byte direct
  `{form_hash, leaf_fn}` entry; the thunk is a V2 optimization (see D8).
- Relaxing any other `aarch64-simulator` requirement. The marker convention,
  tier-bounded import, license headers, `vixl` namespace, and the
  no-`vixl::*`-in-public-headers rule all stay exactly as they are.

## Decisions

### D1. The design doc is authoritative; this artifact decides change-level questions only

**Decision.** `gaby-vm-predecode-cache-design.md` governs cache mechanics. The
decisions below cover only what the design doc leaves open: capability
decomposition, the two capability modifications, build layout, and test
gating. Where a decision is fully settled in the design doc, it is summarized
in one line with a section pointer, not re-argued.

**Why.** `openspec/project.md` explicitly says to link to docs rather than
restate them — duplication drifts. The design doc already weighs alternatives
in depth; copying that into the change record creates two sources of truth.

### D2. One new capability (`predecode-cache`); one capability modified

**Decision.** All new surface — `PredecodeCache`, the `PredecodedEntry` table,
the dual-track `gaby_vm::Simulator`, `ShadowRunner`, the `MemoryWriteSink`
hook — belongs to a new capability, `predecode-cache`. The change additionally
**modifies** one existing capability, `aarch64-simulator` (see D4). Benchmark
integration, which would modify `benchmark-harness`, is split into a separate
follow-up change (see D5).

**Why.** `project.md` states that cross-cutting work — "a future
predecode/dispatch cache, the embedding API, platform-portability layers" —
"should introduce its own capability rather than expanding `aarch64-simulator`".
The marker-bracketed additions to imported files do **not** by themselves
modify `aarch64-simulator`: the relaxed spec already permits them. Only the
test rework changes spec-level behavior of an existing capability.

**Alternatives.** Fold everything into `aarch64-simulator` — rejected; it
violates the project's stated capability boundary and would make one spec own
two very different concerns (imported semantics vs. the cache layer above
them).

### D3. Dual-track execution API, not a runtime gate (design doc §4.1)

**Decision.** `gaby_vm::Simulator` exposes two non-switching execution tracks:
`RunFrom` / `StepOnce` (cache) and `DebugRunFrom` / `DebugStepOnce` (imported
decoder). A hot-path `predecode_cache_active_` bool is **not** used.

**Why** (full rationale in design doc §4.1). A runtime gate would force
mid-stream reconciliation of decoder-only state — `form_hash_`, `last_instr_`,
MOVPRFX-chain tracking — at every switch point; deep-dive risks R1/R2/R4 all
collapse to "no mixed path" once the switch is lifted to the API layer. The
cost is one extra API method, which is a good trade.

### D4. Rework `simulator_correctness` into the dual-path harness; remove its `aarch64-simulator` requirements

**Decision.** `test/simulator_correctness.cc` is reworked: every hand-encoded
sequence runs through both `gaby_vm::Simulator::RunFrom` (cache) and
`DebugRunFrom` (decoder), and post-run state is asserted equal — and equal to
the precomputed expected values. The test moves onto the `gaby_vm` public API;
it no longer needs the privileged `PRIVATE src/` build pattern. At the spec
level this means the two `aarch64-simulator` `simulator_correctness`
requirements are **removed** (each with a `Reason` / `Migration`), and the
dual-path correctness contract is **added** to `predecode-cache`. Coverage is
not lost — it relocates to the capability that owns the cache the test now
exercises.

**Why.** The design doc §4.4.3 calls for exactly this rework. The dual-path
harness *supersedes* the old single-path test: `DebugRunFrom` already
exercises the imported `Decoder → leaf` flow, so a separate test that only
drives `vixl::aarch64::Simulator` would be redundant coverage of the same
path. Removing the `aarch64-simulator` requirements rather than rewriting them
in place avoids a stale "privileged build pattern" requirement title and
avoids two capabilities specifying the same test file.

**Alternatives.** (a) Modify the `aarch64-simulator` requirements in place —
rejected; the test's build pattern and its driver type both change, so the
modified text would contradict its own requirement titles and duplicate the
`predecode-cache` contract. (b) Add a new `predecode_cache_correctness.cc` and
leave `simulator_correctness` untouched — rejected; it produces two
correctness tests with overlapping hand-encoded sequences, one of which
(single-path `vixl::aarch64::Simulator::RunFrom`) is fully redundant with the
new test's `DebugRunFrom` leg.

### D5. Benchmark integration is a separate change

**Decision.** Measuring the cache — running the existing `mixed` / `smoke`
workloads through the cache path and comparing throughput against the decoder
baseline — is **not** part of this change. It is a follow-up change,
`predecode-cache-benchmark`, which modifies the `benchmark-harness`
capability. This change therefore touches only `predecode-cache` (new) and
`aarch64-simulator` (modified).

**Why.** The benchmark is *measurement*, with a **soft** acceptance criterion
(a meaningful improvement, no committed `N×`); the cache itself has **hard**,
falsifiable correctness criteria (dual-path tests green, zero ShadowRunner
divergence). Bundling the two muddies what "done" means for this change. The
benchmark also sequences naturally *after* the cache is proven correct — you
benchmark a correct cache — which matches the project's "correctness first,
then speed" principle. And the seam is clean: the benchmark change consumes
only the *finalized public* `gaby_vm` API this change delivers, so there is no
shared internal surface to coordinate across the two changes.

**Why not split further.** `ShadowRunner` stays in this change: it is the
design's primary correctness oracle (deep-dive R10), and an oracle must ship
*with* the thing it certifies, not chase it. The imported-file hooks stay too —
they are dead code without the cache that uses them. The benchmark engine is
the only V1 piece that is both separable and independently coherent, so it is
the one — and only — split.

### D6. New `src/gaby_vm/` tree; Pimpl public headers

**Decision.** New public headers go under `include/gaby_vm/`
(`predecode_cache.h`, `simulator.h`, `shadow_runner.h`); their implementations
go under a new `src/gaby_vm/` directory and compile into the existing
`gaby_vm::gaby_vm` library. Public headers are Pimpl-wrapped so no `vixl::*`
type appears in `include/gaby_vm/`.

**Why.** This preserves the existing public-header rule (an `aarch64-simulator`
requirement: no VIXL header or `vixl::*` symbol in `include/gaby_vm/`). Pimpl's
indirection cost falls only on construction/destruction and non-hot
accessors — never the execution loop.

### D7. Inherit the design doc's data-structure and boundary decisions wholesale

Most data-structure and boundary choices are settled in the design doc and
adopted without re-litigation. Two are **not** inherited as-is — V1 corrects
the entry layout (D8) and the range-table concurrency model (D9):

- **Shared, append-only cache; `cur_range_` lives on each `Simulator`; entry
  arrays never relocate** — design doc §4.2.2–§4.2.3, §2.2. (The range-table
  concurrency mechanism is corrected — see D9.)
- **Out-of-range PC under `RunFrom` aborts**, no silent fallback — design
  doc §4.3.1.
- **`RegisterCodeRange` is all-or-nothing**; any overlap is rejected; failures
  surface as a C-ABI `RegisterStatus` plus a queryable `ErrorDetail` — design
  doc §4.3.2–§4.3.3.
- **`ShadowRunner` is an always-built API module** (not a compile-time flag),
  lockstep, with a shared stack buffer and the `MemoryWriteSink` hook — design
  doc §4.4.

### D8. V1 uses a 16-byte direct entry, not the 8-byte per-form thunk

**Decision.** Each `PredecodedEntry` is 16 bytes — the instruction's
`form_hash` and its leaf function pointer, stored directly.
`ExecuteInstructionCached` reads both from the entry: it writes `form_hash_`
and calls the leaf. The per-form thunk of design doc §4.2.1 — and its
generated ~2300-instantiation `thunk_table.cc` — is **deferred to V2**. This
supersedes design doc §4.2.1 for V1.

**Why.** The thunk's advantages (half the entry size; `form_hash_` written as
a mov-immediate instead of a load; a V2 hook point for operand
pre-extraction) are real but modest. Its cost is not: ~2300 template
instantiations enumerated at authorship time, a generated source file, and a
bootstrapping step to produce that file from the form→leaf map. For V1 — whose
goal is a *correct* cache proven by ShadowRunner — the 16-byte direct entry
removes the single most intricate mechanism with zero effect on correctness.
The thunk becomes a clean, isolated V2 optimization once the cache is
known-correct.

**Consequences.** Cache memory is 4× code size, not 2× (see the memory risk
below). One extra load per instruction on the `form_hash_` write.
`ExecuteInstructionCached` is a member of the imported `Simulator`, so it
accesses `form_hash_` directly — no `friend` or visibility change is needed.
This resolves what was open question OQ2.

### D9. Correct the range-table concurrency model — reader-writer lock plus stable `CodeRange` records

**Decision.** Design doc §4.2.2 describes the range table as a
`std::vector<CodeRange>` *and* claims lock-free readers. Those contradict, and
the contradiction is a real bug: appending to a `std::vector` can reallocate
its buffer, which (a) invalidates any reader mid-traversal of the table and
(b) dangles every `cur_range_` pointer, since each points into that buffer. V1
corrects this in two parts:

1. `CodeRange` records are held in **stable storage** — a container whose
   elements never relocate on append (e.g. a `std::deque` or a fixed-chunk
   arena) — so a `cur_range_` pointer stays valid for the cache's lifetime.
2. The range *table* is guarded by a single-writer / multiple-reader lock
   (`std::shared_mutex`): `RegisterCodeRange` takes it exclusively; the
   slow-path table search (`LookupEntrySlow`) takes it shared.

The per-instruction **fast path stays lock-free**: a `cur_range_` hit
dereferences only the Simulator-local pointer and the stable `CodeRange` it
names, touching nothing the writer mutates. The lock is paid only on the rare
slow path — a cross-range branch, which design doc §4.2.3 expects to be under
1% of lookups.

**Why a lock, and why not lock-free for V1.** A correct lock-free
append-and-search needs atomic publication, careful memory ordering, and
arguably hazard pointers for reclamation. Because the slow path is rare *by
construction*, a `shared_mutex` there costs almost nothing in aggregate and is
obviously correct. Lock-free range lookup, if ever measured to matter, is a V2
question. Putting the lock on the fast path instead was rejected: a
per-instruction `shared_mutex` acquire is atomic RMW traffic on the exact hot
path the project exists to shrink. This supersedes design doc §4.2.2.

### D10. Nested `RunFrom` is re-entrant via cursor save/restore — no fresh `Simulator`

**Decision.** A native bridge callback — or any code reached from a leaf — may
call `RunFrom` / `DebugRunFrom` / `StepOnce` / `DebugStepOnce` again on the
*same* `Simulator`. Each execution call saves the enclosing run's interpreter
cursor on entry and restores it on return; a RAII snapshot local to the call
makes arbitrary nesting fall out of the C++ call stack. No fresh `Simulator`
per re-entry.

**The cursor** is the interpreter's run-scoped loop and decode state — `pc_`,
the cache `cur_range_`, `form_hash_`, `last_instr_`, and the mid-instruction
`pc_modified_` flag (the implementation confirms the exact minimal set against
the imported `Simulator`'s run-scoped fields). It explicitly does **not**
include the guest register file (X/V/SP/NZCV/…): those are shared mutable
guest state and must flow across a nested call so arguments and return values
pass through, exactly as at a real call boundary.

**Why save/restore, not a fresh `Simulator`.** A fresh `Simulator` per
re-entry would not share the register file or stack, so a native→guest
callback could not pass arguments or observe results without the embedder
marshalling them by hand — and bridge crossings are frequent in the iOS
workload. Cursor save/restore makes `RunFrom` compose like an ordinary
subroutine: guest state flows through, only the interpreter's private cursor
is bracketed.

**Cost and safety.** The save/restore is a handful of words, paid once per
execution call — never on the per-instruction hot path. D9's stable
`CodeRange` records make it sound: a saved `cur_range_` pointer is still valid
after the nested run, even if that run (or another thread) registered new
ranges meanwhile. A nested call MAY use the other track — outer cache, inner
debug, or vice versa — since each call is independent and the cursor set is
track-agnostic.

## Risks / Trade-offs

- **Sizable change (two capabilities, six implementation phases).** →
  `tasks.md` sequences the work so each phase ends at a buildable, test-green
  checkpoint; the dual-path test and ShadowRunner gate correctness
  continuously rather than only at the end. Land it as one PR per task group,
  not a single mega-PR.
- **First markers in the imported tree set a precedent.** → Every marker
  reason names the design doc, as the relaxed `aarch64-simulator` spec
  requires; reviewers treat marker text as code, not as a changelog note.
- **The oracle could itself be wrong (silent ShadowRunner failure).** →
  `shadow_runner_test.cc` injects a deliberate fast-path leaf bug and asserts
  the divergence is caught; an oracle that passes a known-bad input fails the
  build.
- **R1–R12 from the deep-dive.** → Already mapped to V1 resolutions in design
  doc §7; not restated here. The change inherits those resolutions.
- **The cached hot path keeps one polymorphic indirect call.**
  `ExecuteInstructionCached` dispatches through a single call site whose
  target — the entry's leaf function — changes on nearly every instruction. A
  CPU's indirect-branch predictor mispredicts such a site frequently unless
  the guest code is highly repetitive, so branch-misprediction stalls remain a
  real slice of the per-instruction cost. → Accepted for V1: it is still far
  cheaper than the imported decoder's ~5–6 indirect calls plus a heap
  allocation plus two string hashes. Removing this misprediction is the point
  of V2 direct threading (design doc §8); it is also why V1 keeps performance
  acceptance soft.
- **Cache memory is 4× registered code size.** V1's 16-byte direct entry (D8)
  means 4 bytes of guest code cost 16 bytes of cache. Expected patch sizes are
  1–10 MB → 4–40 MB of cache. → Accepted: the embedder owns the budget (design
  doc §4.3.4); V1 adds no cap, LRU, or flush. V2's 8-byte thunk entry would
  halve it.
- **`GetFormToVisitorFnMap` access from the cache.** The populate pass needs
  the `form_hash → leaf` map; how `PredecodeCache` reaches it is OQ1 below.
  (The former `form_hash_`-visibility question is resolved by D8 — no thunk,
  so `ExecuteInstructionCached` accesses the field as an ordinary member.)

## Migration Plan

There is nothing to migrate. The change is purely additive at the API level:
the new `gaby_vm::Simulator` / `PredecodeCache` surface is opt-in, and direct
`vixl::aarch64::Simulator` users (the smoke test, anything embedding the
imported type) are unaffected. Imported-file edits are confined to
self-contained marker regions.

Sequencing follows `tasks.md` phase order. On archive, `openspec archive`
folds the `predecode-cache` delta into a new `openspec/specs/predecode-cache/spec.md`
and applies the `aarch64-simulator` delta to its live spec. Rollback is a
`git revert` of the change; because every imported-file edit is a bracketed
marker region, reverting cannot leave partial drift.

## Open Questions

- **OQ1 — how the cache reaches the `form_hash → leaf_fn` map.**
  `Simulator::GetFormToVisitorFnMap()` is a `static` method returning a
  `const FormToVisitorFnMap*`. If it is reachable from `PredecodeCache`, the
  cache calls it directly; otherwise the cache runs its own `Decoder` plus a
  capture visitor (design doc §7, "额外的"). Both keep the public boundary
  clean; pick the one with less coupling at implementation time.

*(The earlier OQ2, on `form_hash_` visibility for thunks, is closed: D8 drops
the thunk, so `ExecuteInstructionCached` writes `form_hash_` as an ordinary
member and no visibility change is needed.)*
