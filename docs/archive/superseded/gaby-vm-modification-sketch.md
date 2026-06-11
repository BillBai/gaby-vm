# Gaby-VM Modification Sketch

> A directional design note for the Gaby-VM-specific changes on top of
> imported VIXL: predecoded dispatch cache, multi-instance concurrency,
> embedder-allocated stacks, and real atomic semantics. **This is a
> sketch, not an implementation plan.** It establishes terminology and
> the load-bearing constraints. Implementation tasks land in OpenSpec
> changes after this doc is approved.
>
> Citations are paths inside `../vixl/`. The companion architecture and
> dispatch docs cover the upstream surfaces this doc builds on:
> [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md),
> [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md).

> **预解码缓存部分已被取代**：本 sketch 里关于 predecode cache 的具体形状
> （per-instance vs shared cache、`predecode_cache_active_` bool gate、
> operand pre-extraction 的时机、Out-of-range PC fallback 等）已被
> [`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md)
> 取代。其中两条决议被显式翻转——per-instance → shared、bool gate → API 双轨制
> （`RunFrom` / `DebugRunFrom`），新文档 §2.3 有详细记录。
>
> 本 sketch 剩余三块——**多实例并发、embedder stack ownership、real atomic
> semantics**——仍是 authoritative，新文档没有触及。读者读到 cache 段落时请跳
> 到 design doc；读多实例 / 原子 / 栈这三块时本文档仍然有效。

## Restating the goal

Replace the per-iteration upstream path:

```
Decoder::Decode -> CompiledDecodeNode walk
                -> Decoder::VisitNamedInstruction
                -> Simulator::Visit
                -> Simulator::VisitXXX leaf
```

with:

```
cached entry -> Simulator::VisitXXX leaf (or thinned variant)
```

The decode tree walk runs **once per code address at registration
time**, not once per execution. The leaf functions that actually update
guest state remain unchanged; the cache only collapses the dispatch
infrastructure.

Two performance wins: (1) eliminating the per-instruction `Metadata`
heap allocation and double form-name hashing in
`Decoder::VisitNamedInstruction` and `Simulator::Visit`; (2)
eliminating the `std::list<DecoderVisitor*>` iteration on the hot
path, plus the chain of indirect calls.

## Design constraints (load-bearing)

The constraints below are not optional. They are part of the user
contract; downstream design decisions hang on them.

### Single address space with the embedder

The simulator reads guest PC and load/store targets directly as host
pointers. No MMU, no TLB, no shadow address space. This inherits
VIXL's existing direct-host-pointer model (`Memory` class at
`simulator-aarch64.h:371-490`); Gaby-VM does **not** add a translation
layer. Guest code, guest data, host code, host data all share one
address space.

### No self-modifying code

The embedder guarantees that registered instruction memory is
immutable for the lifetime of the simulator instance using it. If
the embedder needs to rewrite code, it must (a) destroy any simulator
instances using that code, (b) rewrite, (c) recreate the simulators
and re-register the code range.

### Pre-allocated predecode buffer

When the embedder hands us a code range, we allocate the matching
predecode buffer in one shot and populate it. No lazy population. No
growth-on-miss. No incremental fill.

### No cache invalidation

There is no invalidate API, no per-page tracking, no coherence
checks. A consequence of the previous three constraints: violating
any of them is undefined behavior, full stop.

### Multi-instance concurrency

The embedder runs **one Simulator instance per host thread**.
Instances share the heap and the predecode buffer (immutable code),
but each instance owns its own register file, PC, NZCV/FPCR, BType,
exclusive-monitor state, and stack. Concurrent guest threads are
realized as concurrent simulator instances on host threads — exactly
like real multi-core hardware.

### Embedder-allocated stacks

Each instance's guest stack is a buffer the embedder provides at
construction time. The simulator does not own the allocation, does
not free it, and does not resize it. Upstream VIXL uses
`SimStack::Allocated` (`simulator-aarch64.h:95-158`) which
self-allocates; Gaby-VM either extends that to accept an external
buffer or wraps the Simulator with a span-based `Memory`. The choice
is flagged in the [extraction map](./vixl-extraction-map.md).

### Real atomic semantics

Because two instances can hit the same guest memory, the following
must produce **correct** cross-thread results, not the probabilistic
single-thread approximation upstream uses for tests:

- Exclusive accesses: LDXR/LDAXR/STXR/STLXR (and pair forms LDXP/STXP).
- LSE atomics: CAS, LDADD, LDSET, LDCLR, LDSMAX, LDSMIN, LDUMAX,
  LDUMIN, SWP — scalar and pair (CASP) variants, with all
  acquire/release/seq_cst suffixes.
- Barriers: DMB, DSB. (ISB stays a no-op for an interpreter.)

This is the single largest correctness change Gaby-VM makes to the
imported VIXL code.

## What stays unchanged

- Most leaf semantics — every `VisitXXX` and `Simulate_*` member
  function for non-atomic, non-barrier instructions, plus the
  `LogicAArch64` helpers that NEON/SVE leaves call.
- The Simulator state shape — registers, vregs, pregs, NZCV, FPCR,
  BType, the direct host-pointer Memory class. Each instance owns its
  own copy.
- `ExecuteInstruction`'s post-decode steps: `last_instr_`,
  `IncrementPc`, `LogAllWrittenRegisters`, `UpdateBType`, the auditor
  assertion (with one tweak — see "auditor side effects" below).
- The branch model: leaves call `WritePc()` to set
  `pc_modified_ = true`, the loop's `IncrementPc()` skips when set,
  and the next iteration looks up the new PC.
- The Decoder is still constructed and used at predecode time. The
  cache *replaces* the per-iteration use of the Decoder, not the
  Decoder itself.

## What changes for multi-instance + atomics

### Exclusive monitor

Replace `SimExclusiveLocalMonitor` and `SimExclusiveGlobalMonitor`
(`simulator-aarch64.h:1217-1281`, both probabilistic) with an
implementation that gives correct cross-thread LL/SC behavior on
shared memory. Sketch:

- LDXR loads the value at the address and records `(addr, value)` in
  the per-instance monitor.
- STXR succeeds iff a host CAS at `addr` against the recorded value
  still observes that value.

This is a *weak* LL/SC that admits ABA only when the underlying value
is rewritten to the same bits — matching the documented weakness of
ARM LL/SC. Acceptable per the ARM ARM and per typical use of
`__atomic_compare_exchange` to emulate LL/SC on x86 hosts.

The global monitor disappears entirely; cross-instance contention is
modeled by real CAS contention, not random failure.

### LSE atomics

Replace today's plain-load/plain-store implementations of CAS / LDADD
/ LDSET / LDCLR / LDSMAX / LDSMIN / LDUMAX / LDUMIN / SWP and their
pair forms with host atomic primitives. Two C++ choices:

- **C++17 baseline:** `__atomic_*` builtins (compiler intrinsics,
  available on GCC and Clang). Aligns with VIXL's existing C++17
  baseline.
- **C++20 if we adopt it:** `std::atomic_ref<T>` for clean memory-order
  selection.

Memory-order mapping for AArch64 atomic suffixes:

| Suffix | Reads | Writes |
|--------|-------|--------|
| (none) | `relaxed` | `relaxed` |
| `A` (acquire) | `acquire` | `relaxed` |
| `L` (release) | `relaxed` | `release` |
| `AL` (seq_cst) | `acquire` | `release` (or `seq_cst` for safety) |

Pair atomics (CASP, LDXP/STXP) require **128-bit atomic load + CAS**
on the host. Available on:

- x86_64: `cmpxchg16b` (with `-mcx16`).
- AArch64 hosts: 16-byte LL/SC via `LDP/STP` plus alignment-checked
  exclusives.

On hosts without 16-byte atomicity, fall back to a global hashed
spinlock keyed on the low bits of the address. Flag this as a
per-host build-time concern — Gaby-VM should add a configure-time
check and either compile the fast path or the lock fallback.

### Barriers

DMB and DSB become real `std::atomic_thread_fence` calls (or
`__atomic_thread_fence`) with the appropriate ordering:

| AArch64 op | Host ordering |
|------------|---------------|
| DMB ISH / DSB ISH | `seq_cst` (or `acq_rel` if we can prove no SC need) |
| DMB ISHLD | `acquire` |
| DMB ISHST | `release` |
| ISB | no-op (we don't reorder around it) |

This is conservative; we can tighten as profiling reveals slack.

### Stack ownership

Either:

- **(a) Extend `SimStack::Allocated`** so its constructor accepts an
  externally-owned `(base, size)` pair. Cleaner; one small upstream-style
  patch.
- **(b) Replace the stack member of `Memory`** with a Gaby-VM-owned
  span type. Zero local diffs on imported VIXL files; more
  boilerplate in Gaby-VM.

Either way, **the embedder's buffer pointer and length are part of
the constructor signature**. The simulator never calls `malloc` for
stack memory.

## What new structures we likely need

### Predecoded entry (the cache row)

POD struct, fixed size, ideally cache-line-friendly:

```cpp
// Sketch only — final layout decided at implementation time.
struct PredecodedEntry {
  uint32_t form_hash;            // restored into Simulator::form_hash_
                                 // before leaf call.
  void (*leaf_fn)(Simulator*, const Instruction*);
  uint32_t raw_word;             // optional: keeps Instruction* deref off the hot path
  // optional: pre-extracted operand bundle for hot families
};
```

For hot families (e.g. `AddSubImmediate`: `Rd, Rn, imm12, shift`) we
can attach pre-extracted operand bundles in a follow-up pass, but the
first cut should be the minimal `(form_hash, leaf_fn)` pair.

### Code-range descriptor

Provided by the embedder at registration time:

```cpp
struct CodeRange {
  const Instruction* start;   // host pointer to the first 32-bit word
  size_t             size;    // bytes; must be a multiple of 4
};
```

The simulator allocates one predecode array of `size / 4` entries and
populates it in a single pass. Layout: a flat
`std::vector<PredecodedEntry>` (or `std::unique_ptr<PredecodedEntry[]>`)
indexed by `(pc - start) >> 2`. No hash map. No tree.

### Registration API

Sketch only:

```cpp
class Simulator {
 public:
  // ...
  void RegisterCodeRange(const Instruction* start, size_t size_bytes);
  // ...
};
```

Or a free helper that wraps construction:

```cpp
class GabyVMSimulator {
 public:
  GabyVMSimulator(Decoder* decoder,
                  void* stack_base, size_t stack_size,
                  FILE* stream = stdout);
  void RegisterCodeRange(const Instruction* start, size_t size_bytes);
  // forwards Run, RunFrom, etc.
};
```

The embedder calls `RegisterCodeRange` for each region of immutable
guest code before the first `RunFrom`. The simulator may keep a
small sorted table of registered ranges to translate `pc_` → entry
index in `O(log N)` for N registered ranges (typically very small).

### Out-of-range fallback

When `pc_` lies outside any registered range, two design options:

- **(a) Hard error.** Abort with a diagnostic. Safer default given
  the constraint that the embedder owns the lifetime contract.
- **(b) Fall back to legacy `decoder_->Decode(pc_)`.** Useful for
  bring-up and tests where embedders may not have pre-registered
  every reachable address.

Doc default: **(a)** for production; **(b)** behind a debug build flag
for bring-up. Final choice belongs to the implementation change.

## Where the cache plugs in

### At registration time

For each `(pc, code_word)` in the registered range:

1. Run the existing decode + visitor lookup *without executing the
   leaf*. Concretely: invoke a "predecode visitor" that captures the
   form hash and the leaf function pointer instead of calling the
   leaf. The visitor list ordering means the
   `CPUFeaturesAuditor::Visit()` runs in this same pass, exposing
   any unsupported instruction.
2. Record the instruction's required feature set into the entry. If
   any instruction in the range is unavailable on the configured CPU
   profile, fail registration loudly — the embedder gets a clean
   error rather than a silent abort mid-run.
3. Store `(form_hash, leaf_fn_ptr)` (and optional operand bundle)
   into the predecode array.

This pass is single-threaded per Simulator instance. Multiple
instances can register the same code range independently and produce
identical entries; whether to share one predecode array across
instances or one per instance is an implementation choice — sharing
is fine because the array is read-only after registration.

### At run time

`ExecuteInstruction()` becomes (sketch only):

```cpp
void Simulator::ExecuteInstruction() {
  VIXL_ASSERT(IsWordAligned(pc_));
  pc_modified_ = false;
  // BType / guarded-page check stays.

  const PredecodedEntry& e = predecode_for(pc_);
  form_hash_ = e.form_hash;          // load-bearing — leaves read this.
  e.leaf_fn(this, pc_);

  last_instr_ = ReadPc();
  IncrementPc();
  LogAllWrittenRegisters();          // no-op when trace off.
  UpdateBType();
  // CPU-features check absorbed into RegisterCodeRange — see below.
}
```

No decoder walk. No `Metadata` allocation. No visitor list iteration.
One array load + one indirect call.

## Major correctness risks

### form_hash dependence in shared leaves

Several `Simulate_*` leaves are shared across multiple forms and
switch on `form_hash_` to choose behavior — see
[`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md)
section 10 (`Simulate_PdT_PgZ_ZnT_ZmT` at
`simulator-aarch64.cc:2325-2344`). The cached path **must** restore
`form_hash_` before each leaf call, even when it looks redundant.
Default: always restore from the cached entry.

### Auditor side effects

`cpu_features_auditor_.InstructionIsAvailable()` is asserted at the
bottom of every `ExecuteInstruction()` (`simulator-aarch64.h:1441`).
Two options for the cached path:

- **(a) Pre-approve at registration time.** Run the auditor's
  `Visit()` once per instruction during `RegisterCodeRange`; if any
  fails, reject registration. The hot path then skips the assertion.
  Aligns with the no-invalidation constraint and is faster.
- **(b) Replay per execution.** Call the auditor's `Visit()` from the
  hot path. Preserves per-execution feature counters but pays the
  auditor's cost on every replay.

Doc default: **(a)**. The loss of per-execution accounting is
acceptable; the auditor's accumulating "seen features" set is
populated correctly during registration.

### Trace fidelity

`PrintDisassembler` runs before the leaf when trace flags are
non-zero. Default cache-path behavior: when trace is enabled, fall
through to the legacy slow path (the original `decoder_->Decode(pc_)`
plus visitor list). The cache hot path is then untraced by
construction; trace output matches upstream VIXL byte-for-byte.

### Branches and PC arithmetic

`WritePc()` mutates `pc_` and sets `pc_modified_ = true`
(`simulator-aarch64.h:1369-1374`). Cache lookup happens *before* the
leaf runs; the loop's post-leaf step picks up the new `pc_`
naturally. A branch into an unregistered range hits the
out-of-range fallback above.

### Atomics — implementation correctness

The replaced exclusive monitor and LSE visitors are the load-bearing
correctness change. Risks:

- **Weak-ABA on STXR-via-CAS.** Document this as an intended deviation
  from a cache-coherence-based monitor. Acceptable per ARM ARM.
- **Pair atomics need 16-byte host atomicity.** On x86_64 with
  `-mcx16` and on AArch64 hosts, native. On other hosts, fallback to
  a global hashed spinlock. Configure-time check required.
- **Per-leaf testing.** Multi-threaded stress benchmark must be a
  correctness gate, not just a perf measurement. See
  [`baseline-benchmark-suite.md`](./baseline-benchmark-suite.md) for
  the test design.

### Multi-instance state isolation

Each Simulator instance owns its register file, PC, NZCV/FPCR,
exclusive-monitor record, BType, stack, output stream, and PRNG
seed. Risks:

- Any place that touches *shared* state must be made explicitly
  per-instance or thread-safe. Notable suspects:
  - `placeholder_pipe_fd_[2]` opened in the constructor
    (`simulator-aarch64.cc:657`) — already per-instance, fine.
  - `<mutex>` is included by `simulator-aarch64.h`; suggests at least
    one lock somewhere (likely in `MetaDataDepot` for MTE tag-map
    updates). Audit before declaring the multi-instance work done.
  - The static `Simulator::GetFormToVisitorFnMap()` is read-only
    after first call; safe.
  - The static register name tables (`xreg_names`, etc., h:5361-5369)
    are read-only; safe.
- Trace output: each instance writes to its own `FILE* stream_`.
  Embedders are responsible for ensuring streams aren't shared
  unsynchronized; document this.

### Stack aliasing

Different instances must be given non-overlapping embedder-provided
stack buffers. We cannot enforce this at runtime cheaply — declare
it an embedder responsibility. Provide a debug-build assertion on
construction that flags overlap with already-registered stacks.

### Self-test (the critical correctness gate)

Cached vs uncached execution must be **byte-exact** in register and
memory state for any program. Implementation detail:

- A debug build flag (e.g. `GABY_VM_DOUBLE_DECODE`) runs both paths
  per instruction and `VIXL_CHECK`s the resulting state.
- For atomics specifically, also run a multi-instance stress test:
  N threads, each running a Simulator instance, all incrementing a
  shared counter via guest LDXR/STXR (and separately via LDADD,
  CAS). Final value must equal `N × iterations`. Catches monitor
  and host-atomic implementation bugs early.

## Future optimization directions

These are out of scope for the initial cache work. Listed only so we
do not surprise ourselves later by reinventing them.

- **Threaded interpretation.** The leaf returns a pointer to the next
  entry; the loop becomes
  `for (entry = first; entry; entry = entry->run(state))`. Requires
  changing the leaf signature, so it's a deeper refactor. Worthwhile
  if profiling shows the dispatch cost still dominates after the
  cache.
- **Specialized leaves.** Per-form leaves that consume the
  pre-extracted operand bundle directly (no `instr->GetRd()` calls on
  the hot path). Big win for hot integer-ALU forms.
- **Basic-block linking.** At predecode time, follow straight-line
  code and record per-entry "next entry" pointers. Branches still
  resolve through the address→entry lookup. A weak form of this for
  free is "just keep the index — `++entry` works for sequential
  PCs".
- **Profile-guided leaf inlining for hot families.** Very late stage;
  PGO build flag selects whether to expand a few hot leaves into the
  loop body.

## Non-goals

- MMU / TLB.
- QEMU-style TCG or any other intermediate representation.
- Any RWX or `mprotect` use.
- JIT fallback path or runtime code generation.
- Self-modifying code support.
- Cache-invalidation API.
- Sharing register state between simulator instances. Each instance
  is single-threaded; cross-instance synchronization happens **only**
  through guest atomic instructions on shared memory, exactly like
  real multi-core hardware.

## Where to read next

- [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md)
  — the upstream surface this design extends.
- [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md)
  — what the cache replaces and what it must preserve.
- [`vixl-extraction-map.md`](./vixl-extraction-map.md) — file list,
  including which simulator files are expected to be modified after
  import.
- [`baseline-benchmark-suite.md`](./baseline-benchmark-suite.md) —
  how we measure the resulting speedup and validate atomics
  correctness.
