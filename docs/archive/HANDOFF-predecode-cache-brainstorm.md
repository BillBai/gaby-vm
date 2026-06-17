# Predecode / Decode Cache Brainstorm Handoff

> **Status:** This handoff was written after an incomplete brainstorm session.
> It records the decisions already agreed, the items that still needed an
> explicit decision, and the open questions that had not yet been discussed.
>
> **Not an OpenSpec change.** The intended output was a normal design document
> under `docs/refs/`, at the same level as
> [`gaby-vm-modification-sketch.md`](superseded/gaby-vm-modification-sketch.md)
> and
> [`vixl-fetch-decode-dispatch-deep-dive.md`](../refs/vixl-fetch-decode-dispatch-deep-dive.md).
> An OpenSpec change would only come later, when implementation was ready.

## Background

Gaby-VM's main optimization direction is:

```text
predecode once -> cache decoded dispatch target -> execute cached path repeatedly
```

The upstream VIXL dispatch path costs roughly hundreds of cycles per
instruction in the profiled hot path. The real leaf operation is only a small
part of that cost. The large costs are the `Metadata` allocation and repeated
string/hash dispatch in `Decoder::VisitNamedInstruction`, the visitor-list walk,
and the `form_hash -> leaf_fn` lookup in `Simulator::Visit`.

The predecode cache removes that repeated chain. A cache hit should become one
array lookup plus one indirect call. The leaf semantics remain VIXL semantics.
Because iOS is a primary target, cache entries are ordinary data, not generated
code.

## Agreed Decisions

### V1 scope: minimal hit replacement

V1 only caches `(form_hash, leaf_fn)`.

`ExecuteInstruction` becomes approximately:

```cpp
lookup entry -> write form_hash_ -> call leaf_fn
```

Operand pre-extraction, basic-block linking, and direct threading are left for
later versions.

### Population model: preheat, no lazy fill, no invalidation

The embedder registers all executable sections from Mach-O patch dylibs when a
patch package is loaded, then preheats those ranges. Runtime loading of new
patches may still happen, but it is expected to be low frequency, so
`RegisterCodeRange` must be callable after initial setup. Self-modifying code is
not supported.

### Workload model: iOS patch package system

- One patch package contains multiple Mach-O dylibs.
- Multiple `Simulator` instances run concurrently, one per host thread.
- All instances share a read-only cache.
- VM and native execution interleave frequently.
- Native calls use VIXL's existing `RegisterBranchInterception` mechanism inside
  the leaf path; the cache hot path does not need special handling.
- A process is expected to have tens to hundreds of registered code ranges.

### Cache ownership: standalone `PredecodeCache`

`gaby_vm::PredecodeCache` is an independent object controlled by the embedder.
It can be shared by many `Simulator` instances. Once registered, cache contents
are read-only, so sharing the cache does not introduce write races.

### VIXL import boundary policy was relaxed

Imported VIXL files no longer need to remain byte-identical to upstream. Changes
are allowed when they are documented with comments so future audits can review
them. The OpenSpec simulator requirements should be kept consistent with that
policy when they are next updated.

## Recommendations From The Prior Session

These were recommendations, not explicit final decisions. The next design
session should re-confirm them before treating them as settled.

### PC-to-entry lookup: multiple ranges plus cached current range

Use a fast current-range check before falling back to a range-table lookup:

```cpp
inline PredecodedEntry* LookupEntry(const Instruction* pc) {
  if (VIXL_LIKELY(cur_range_)) {
    uintptr_t off = uintptr_t(pc) - uintptr_t(cur_range_->start);
    if (VIXL_LIKELY(off < cur_range_->size_bytes)) {
      return &cur_range_->entries[off >> 2];
    }
  }
  return LookupEntrySlow(pc);
}
```

Sequential AArch64 execution and intra-function branches should keep the
current-range hit rate high. Cross-dylib branches pay the slow lookup once and
then return to the fast path.

### Entry layout: 8-byte thunk pointer

Store only a per-form thunk pointer:

```cpp
struct PredecodedEntry {
  void (*thunk)(Simulator*, const Instruction*);
};
```

Each thunk writes the form hash as a compile-time constant, then tail-calls the
real VIXL leaf:

```cpp
template <auto MemberFn, uint32_t FormHashConstant>
void PredecodeThunk(Simulator* sim, const Instruction* instr) {
  sim->form_hash_ = FormHashConstant;
  (sim->*MemberFn)(instr);
}
```

The motivation was a smaller entry, one fewer hot-path load for `form_hash_`,
and a natural hook for later operand pre-extraction.

### Public API: Pimpl isolation

The API should hide VIXL types from embedders:

```cpp
namespace gaby_vm {
class PredecodeCache {
 public:
  PredecodeCache();
  ~PredecodeCache();
  PredecodeCache(const PredecodeCache&) = delete;
  void RegisterCodeRange(const void* start, size_t size_bytes);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace gaby_vm
```

`Simulator` should similarly expose a stable embedding surface while keeping the
VIXL implementation behind an `Impl`.

### File layout

The suggested layout was:

```text
include/gaby_vm/
  predecode_cache.h
  simulator.h

src/gaby_vm/
  predecode_cache.cc
  simulator.cc
  thunks.h
  thunk_table.cc

src/aarch64/
  simulator-aarch64.h
  simulator-aarch64.cc
```

### `ExecuteInstruction` gating

The proposed V1 shape was a hot-path bool gate:

```cpp
if (VIXL_LIKELY(predecode_cache_active_)) {
  DispatchViaPredecodeCache();
} else {
  decoder_->Decode(pc_);
}
```

`predecode_cache_active_` would be false when no cache is attached, tracing is
enabled, extra decoder visitors are registered, or debugger mode is active.

This recommendation was later superseded by the public dual-track API
`RunFrom` / `DebugRunFrom`.

## Decision Items Still Open

### Out-of-range PC

Prior recommendation: production builds abort with a clear "PC is not in any
registered code range" diagnostic; debug builds may optionally allow a fallback
to `decoder_->Decode(pc_)` behind a compile-time flag.

Open question: whether aborting is too strict for bridge scenarios, and whether
VM-to-native control flow can legitimately leave the simulator with an
out-of-range PC.

### Decoder lifetime

Prior recommendation: `PredecodeCache::Impl` owns the `vixl::aarch64::Decoder`
used during `RegisterCodeRange`. The cache and decoder share a lifetime.
`Simulator::Impl` does not own a separate decoder except through fallback/debug
access exposed by the cache.

### CPU feature auditor

Prior recommendation: the cache owns a `CPUFeaturesAuditor` and checks
availability at registration time. Unsupported instructions cause
`RegisterCodeRange` to return an error. The hot path should not repeat auditor
assertions after successful registration.

Open questions: whether embedders can allow partial unsupported ranges, and
what error representation should be used.

### Shadow self-test

Prior recommendation: add a compile-time `GABY_VM_DOUBLE_DECODE` mode that runs
both cache and decoder paths and aborts on a register, flag, PC, or memory-write
difference. It should be off by default and zero-cost in normal builds.

Open questions: whether this belongs in V1 and exactly which state must be
compared.

## Wider Open Questions

- Concurrency during `RegisterCodeRange`: stop-the-world, RCU-like append-only
  table, or locking.
- Cross-range branch semantics and slow lookup data structure.
- Native bridge return semantics and cache races while native code is running.
- Cache memory budget, out-of-memory behavior, and whether a range-unload API is
  needed.
- Test integration with the existing correctness and benchmark suites.
- Diagnostics for registration failures, out-of-range PCs, and shadow diffs.
- Whether `PredecodeCache` construction should take CPU features.
- Concrete performance targets and benchmark acceptance criteria.

## Documents To Read First

1. [`docs/archive/superseded/gaby-vm-modification-sketch.md`](superseded/gaby-vm-modification-sketch.md)
   for the overall direction.
2. [`docs/refs/vixl-fetch-decode-dispatch-deep-dive.md`](../refs/vixl-fetch-decode-dispatch-deep-dive.md)
   for performance analysis and risks.
3. `docs/refs/vixl-decode-dispatch-pattern.md` for upstream VIXL control flow.
4. `docs/refs/vixl-aarch64-simulator-architecture.md` for simulator subsystem
   context.
5. [`AGENTS.md`](../../AGENTS.md) and [`docs/architecture.md`](../architecture.md)
   for project-level constraints.
6. [`openspec/specs/aarch64-simulator/spec.md`](../../openspec/specs/aarch64-simulator/spec.md)
   when normative requirements need to be updated.

## Next-Session Notes

- This is not an OpenSpec change. Produce a normal `docs/refs/` design document
  first.
- Reconfirm the prior recommendations before treating them as committed
  decisions.
- Explain thunks plainly: a thunk is a small forwarding function that writes the
  compile-time form hash into the simulator and then calls the real leaf.
- Give real options when a design point is still open.
- The first five decision sections above were explicitly agreed; the later
  recommendations were not explicitly finalized.

## Intended Design Doc

The intended document was
`docs/refs/gaby-vm-predecode-cache-design.md`, structured around goal,
constraints, unchanged semantics, changed structures, integration points, risks,
and non-goals.
