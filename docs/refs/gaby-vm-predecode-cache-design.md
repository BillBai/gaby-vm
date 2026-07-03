# Gaby-VM Predecode Cache Design

[Chinese version](gaby-vm-predecode-cache-design.zh-cn.md)

This is the authoritative design note for gaby-vm's predecode/dispatch cache.
It turns the project direction, "predecode once, cache the dispatch target, run
the cached path repeatedly", into the concrete V1 shape: API split, data
structures, imported-file insertion points, correctness risks, and the
measurement record.

It is not an OpenSpec change, a spec delta, or an implementation plan. Source
references use repository-relative paths.

## 1. Goal

VIXL's upstream interpreter spends hundreds of cycles per instruction finding
the semantic leaf:

```text
ExecuteInstruction
  -> Decoder::Decode(pc_)
  -> CompiledDecodeNode tree walk
  -> Decoder::VisitNamedInstruction
       Metadata allocation, form hash, unallocated check, visitor fan-out
  -> Simulator::Visit
       form hash again, FormToVisitorFnMap lookup
  -> leaf function
```

gaby-vm moves the repeated work to registration time:

```text
RegisterCodeRange
  -> decode each 4-byte word once
  -> store form_hash, flags, and leaf dispatcher in a flat entry array

ExecuteInstructionCached
  -> find CodeRange
  -> load PredecodedEntry
  -> write form_hash_
  -> call the cached leaf
```

The semantic leaf code stays VIXL's leaf code wherever practical. The cache is
ordinary data and does not allocate executable memory.

## 2. Design Constraints

### 2.1 Inherited Constraints

These come from `gaby-vm-modification-sketch.md`, `docs/architecture.md`, and
the current embedding model:

- Single address space. A guest PC is a host pointer, so cache lookup can use
  host pointer arithmetic directly.
- No self-modifying code in V1. The embedder guarantees registered code bytes do
  not change while the cache is in use.
- No JIT and no RWX memory. Predecoded data is POD data, not generated code.
- Multiple simulator instances may run concurrently. Each simulator owns its
  registers, PC, NZCV, FPCR/FPSR, BType, exclusive monitor state, and stack.
- The embedder provides the simulator stack buffer.

### 2.2 V1 Decisions

- A `PredecodeCache` may be shared across simulator instances. The execution
  path is read-only and lock-free on range hits.
- The cache is append-only. `RegisterCodeRange` can add ranges during the cache
  lifetime, but V1 has no unregister, flush, or invalidate operation.
- Overlapping ranges are errors. The embedder owns de-duplication.
- The API has two execution modes:
  - `RunFrom` and `StepOnce` use the cache path.
  - `DebugRunFrom` and `DebugStepOnce` use VIXL's original decoder path.
- A simulator with a null cache is valid for decoder mode only.
- `ShadowRunner` is part of V1. It locksteps cache mode against decoder mode and
  compares registers and memory writes.
- Performance acceptance is measured, but no hard `Nx` number is committed.
  Correctness guard rails matter more than headline speed.

### 2.3 Superseded Older Notes

This design supersedes two older choices:

| Older note | V1 decision | Reason |
|------------|-------------|--------|
| Per-instance cache was left as an implementation choice. | Shared cache is required. | Large iOS patch payloads can be hundreds of MB. Per-instance caches multiply memory by thread count. |
| Trace mode could fall through to a legacy slow path. | Use separate `RunFrom` and `DebugRunFrom` APIs. | Runtime switching makes `form_hash_`, `last_instr_`, and MOVPRFX state hard to keep coherent. Separate loops avoid that class of bug. |

## 3. What Stays the Same

The cache replaces dispatch infrastructure, not instruction semantics.

Unchanged mechanisms:

- VIXL leaf functions such as `Simulator::VisitXxx` and `Simulate_*`.
- Simulator state layout: general registers, vector registers, predicates,
  NZCV, FPCR, FPSR, BType, and PC.
- The direct host-pointer memory model.
- `ExecuteInstruction` pre/post semantics: `pc_modified_`, BType checks,
  guarded-page checks, `last_instr_`, `IncrementPc`, register logging, and
  `UpdateBType`.
- `CPUFeaturesAuditor` as a class. V1 uses it during predecode rather than in
  the cache hot path.
- Native branch interception inside leaves. If a branch hook redirects PC to an
  unregistered address, cache mode aborts rather than falling back.
- The decoder still exists for cache population and for decoder mode.

## 4. V1 Shape

### 4.1 Dual-Mode API

The public wrapper exposes two modes through `gaby_vm::Simulator`:

```cpp
Simulator(PredecodeCache* cache, void* stack_buf, size_t stack_size);

void RunFrom(uintptr_t entry_pc);
void DebugRunFrom(uintptr_t entry_pc);

bool StepOnce();
bool DebugStepOnce();
```

`RunFrom` requires a cache and executes only registered ranges. `DebugRunFrom`
uses the imported VIXL decoder and can run without a cache. Trace/debugger
features and custom visitors belong to decoder mode.

A single API with an internal "cache enabled" bool was rejected. The bool itself
would be cheap, but switching paths mid-run would need repair logic for
decoder-adjacent state. Keeping each run on one loop removes that problem.

### 4.2 Data Structures

#### 4.2.1 `PredecodedEntry`

The implemented V1 entry is exactly 16 bytes:

```cpp
struct PredecodedEntry {
  uint32_t form_hash;
  uint32_t flags;
  const void* leaf;
};
```

`form_hash` is written to the imported simulator's `form_hash_` before the leaf
call. Shared `Simulate_*` leaves branch on that value.

`flags` stores hot-path classifications computed once by predecode. Bit 0 marks
BTI-relevant instructions, so most cache-mode instructions skip the guarded-page
BType check. Remaining bits are reserved for future per-form predecode work.

`leaf` is an opaque handle to the resolved cache-track dispatcher. The public
header exposes no `vixl::` type.

> Superseded by `cache-dispatch-devirt` (C1, 2026-07-03): `leaf` now holds a
> statically-bound *handler* function pointer, not a pointer to
> pointer-to-member-function storage. The field size, offset, and type are
> unchanged (still `const void*` in the 16-byte entry); only the pointed-at
> thing changed. The cache-track dispatch is now one handler-slot load plus one
> indirect call — no pmf materialisation, no vtable walk.

The older 8-byte per-form thunk idea is deferred. Direct 16-byte entries were
chosen for V1 because they are simpler, auditable, and already fast enough to
validate the dispatch-cache architecture.

> Superseded by `cache-dispatch-devirt` (C1): the "already fast enough, thunk
> deferred" stance no longer holds — per-form thunks landed. The entry stayed
> 16 bytes (the 8-byte-entry variant is still not pursued), but each form now
> resolves to a macro-generated generic thunk (a `static` member of
> `vixl::aarch64::Simulator`) that seats `form_hash_`, runs the BTI / MOVPRFX
> protocol, makes a qualified — hence statically bound — call to the imported
> leaf, and runs the epilogue. This removed the virtual-dispatch dependent-load
> tail the old `(this->*pmf)(pc_)` path paid, and fixes the handler ABI the
> later specialization changes (C2-C7) build on. See the change's design.md and
> `docs/refs/gaby-vm-fast-dispatch-synthesis-2026-07-03.md`.

#### 4.2.2 `CodeRange` and Range Table

Each registered range owns a flat `entries[]` array with one entry per 4-byte
instruction word:

```cpp
struct CodeRange {
  uintptr_t start;
  size_t size_bytes;
  const PredecodedEntry* entries;
};
```

The cache stores ranges in stable storage and never relocates a created range.
This is the key append-only invariant: a simulator can cache a `CodeRange*`
without generation counters or hazard pointers.

`RegisterCodeRange` serializes writers. Already-registered ranges remain safe
for concurrent readers.

#### 4.2.3 PC to Entry Lookup

Each imported `vixl::aarch64::Simulator` instance carries a cached
`cur_range_`. The hot path first checks that range:

```cpp
uintptr_t off = pc_addr - cur_range_->start;
if (off < cur_range_->size_bytes) {
  return &cur_range_->entries[off >> 2];
}
```

On a miss, the simulator calls `PredecodeCache::FindRange`, which takes the
range table's shared lock, binary-searches the sorted ranges, and updates
`cur_range_`.

`cur_range_` belongs to the simulator, not to the cache. It is mutable execution
state tied to one PC stream.

### 4.3 Boundary Cases

#### 4.3.1 Out-of-Range PC

Cache mode aborts when PC is outside every registered range. It does not fall
back to decoder mode. The embedder selected `RunFrom`, so an out-of-range PC is
an embedder registration bug or an invalid branch target. Hard failure is easier
to diagnose than a silent slowdown.

#### 4.3.2 `RegisterCodeRange` Errors

`PredecodeCache::RegistrationStatus` returns:

- `Ok`
- `InvalidArgument`
- `OverlappingRange`
- `UnsupportedFeature`
- `OutOfMemory`

`GetLastRegistrationError()` returns structured detail for the most recent
failed call.

All-or-nothing now applies to structural registration failures: invalid size,
overlap, or allocation failure. Earlier drafts also rejected a range for
per-word decode properties such as unallocated encodings. Current V1 no longer
does that. The cache can register ranges containing data words by using sentinel
entries, described in R12.

`UnsupportedFeature` remains in the ABI for stability. The current predecode
path uses `CPUFeatures::All()` and normally does not produce this status.

#### 4.3.3 Overlapping Ranges

Any overlap is `OverlappingRange`, including an exact duplicate. Append-only
storage has no cheap overwrite semantics, and accepting duplicate registrations
would hide embedder bugs.

#### 4.3.4 Memory Budget

The embedder owns the budget. V1 has no cap, LRU, or flush. Each 4-byte
instruction maps to a 16-byte entry, so cache memory is roughly 4x code size.
The shared cache avoids multiplying that cost by the number of simulator
instances.

### 4.4 ShadowRunner

`ShadowRunner` is the V1 correctness oracle. It owns two simulators:

- `fast_`: cache mode, backed by a `PredecodeCache`;
- `ref_`: decoder mode, constructed with a null cache.

It mirrors initial register writes, then runs the two simulators in lockstep.
After every instruction it compares state and reports the first divergence.

Lockstep is intentional. Comparing only final state can hide the first wrong
instruction behind later behavior.

#### 4.4.1 Compared State

V1 compares:

| Class | Fields |
|-------|--------|
| General registers | X0-X30 and SP |
| FP/SIMD | V0-V31, full 128-bit values |
| PC | current PC |
| Flags/control | NZCV, FPCR, FPSR, BType |
| Memory writes | per-step write trace |

V1 does not compare SVE Z/P/FFR state or exclusive-monitor internals directly.
Exclusive-monitor behavior is still observed through architectural result
registers such as STXR success/failure.

#### 4.4.2 Memory Write Trace

ShadowRunner needs a memory oracle without giving the two simulators different
stack addresses. The imported simulator therefore has a marker-guarded optional
write sink:

```cpp
class MemoryWriteSink {
 public:
  virtual ~MemoryWriteSink() = default;
  virtual void Record(uintptr_t addr, size_t size,
                      uint64_t value_lo, uint64_t value_hi) = 0;
};
```

Each step clears both trace buffers, runs one instruction on each mode, then
compares the recorded writes by address, size, and value.

The sink is in `vixl::aarch64` because the hook point is inside imported
`Simulator::MemWrite`. Putting it in `gaby_vm` would create an awkward reverse
dependency from imported VIXL code into public gaby-vm headers.

#### 4.4.3 Test Integration

- `simulator_correctness` runs both modes.
- `shadow_runner` tests ShadowRunner's report path and intentional divergence
  detection.
- `workload_shadow` runs committed benchmark workloads through ShadowRunner.
- Benchmarks do not use ShadowRunner by default because lockstep execution is
  intentionally slow.

### 4.5 Performance Acceptance

V1 acceptance:

1. `bench_baseline` and `bench_smoke` show a meaningful measured cache-mode
   improvement over decoder mode.
2. correctness tests pass across cache mode and decoder mode.
3. ShadowRunner reports zero divergence on committed workloads.

No hard speed multiplier is part of the design contract.

## 5. Implemented Structures

### 5.1 `gaby_vm::PredecodeCache`

Location:

- `Sources/gaby_vm/include/gaby_vm/predecode_cache.h`
- `Sources/gaby_vm/src/gaby_vm/predecode_cache.cc`

Responsibilities:

- own cache lifetime;
- register code ranges;
- run the one-time predecode pass;
- own stable `CodeRange` and entry storage;
- expose `FindRange` to the cache execution path.

### 5.2 `gaby_vm::PredecodedEntry`

Location: `Sources/gaby_vm/include/gaby_vm/predecode_cache.h`.

Responsibility: one flat, opaque, 16-byte slot per instruction word. See
section 4.2.1.

### 5.3 `gaby_vm::PredecodeCache::CodeRange`

Location: `Sources/gaby_vm/include/gaby_vm/predecode_cache.h`.

Responsibility: stable range metadata plus an entry array. See sections 4.2.2
and 4.2.3.

### 5.4 `RegistrationStatus` and `RegistrationError`

Location: `Sources/gaby_vm/include/gaby_vm/predecode_cache.h`.

Responsibility: simple ABI-stable status plus optional detail. See section
4.3.2.

### 5.5 `gaby_vm::Simulator`

Location:

- `Sources/gaby_vm/include/gaby_vm/simulator.h`
- `Sources/gaby_vm/src/gaby_vm/simulator.cc`

Responsibilities:

- public Pimpl wrapper around imported `vixl::aarch64::Simulator`;
- connect a `PredecodeCache` to the imported simulator;
- expose cache and decoder run/step APIs;
- keep public headers free of `vixl::` types.

### 5.6 `gaby_vm::testing::ShadowRunner`

Location:

- `Sources/gaby_vm/include/gaby_vm/shadow_runner.h`
- `Sources/gaby_vm/src/gaby_vm/shadow_runner.cc`

Responsibilities: lockstep execution, state comparison, memory-write comparison,
and divergence reporting.

### 5.7 `vixl::aarch64::MemoryWriteSink`

Location: `Sources/gaby_vm/src/aarch64/simulator-aarch64.h`, inside a
marker-guarded imported-file edit.

Responsibility: optional per-step memory write observation for ShadowRunner.

## 6. Imported-File Insertion Points

Every imported-file edit must have a marker comment explaining why the change
exists and pointing back to this document.

### 6.1 `Sources/gaby_vm/src/aarch64/simulator-aarch64.h`

Key edits:

- `MemoryWriteSink`, `write_sink_`, and `SetMemoryWriteSink`.
- `SetPredecodeCache`.
- `ExecuteInstructionCached`.
- `StepOnce` and `DebugStepOnce`.
- cache-mode `cur_range_`.
- `ResolvePredecodeLeaf`.
- re-entrant cursor save/restore for nested branch-hook execution.
- memory write sink calls in `MemWrite`.

### 6.2 `Sources/gaby_vm/src/aarch64/simulator-aarch64.cc`

The decoder-mode `Simulator::Visit` path stays intact. Cache population resolves
form hashes through a small public forwarding seam rather than exposing the
whole visitor map to gaby-vm code.

### 6.3 `Sources/gaby_vm/include/gaby_vm/`

Public gaby-vm headers expose clean `gaby_vm::` API types only. They do not
include imported VIXL headers.

### 6.4 `Sources/gaby_vm/src/gaby_vm/`

Owned implementation files contain the Pimpl glue, cache registration logic,
and ShadowRunner.

### 6.5 Marker Comment Style

Marker comments should explain the reason for the edit, not just restate what
the code does. A good marker names this document and the relevant section.

Example shape:

```cpp
// gaby-vm BEGIN:
// Adds the cache-hit execution path. The original ExecuteInstruction remains
// the decoder-mode path; the two loops never switch mid-run, which keeps
// form_hash_ / last_instr_ / MOVPRFX state coherent.
// See docs/refs/gaby-vm-predecode-cache-design.md section 4.1.
...
// gaby-vm END
```

## 7. Risks: R1-R12 From the Deep Dive

| # | Risk | V1 handling |
|---|------|-------------|
| **R1** | `form_hash_` must be correct before the leaf call. | Each entry stores `form_hash`; cache mode writes it before calling `leaf`. |
| **R2** | MOVPRFX chaining depends on previous-instruction state. | Separate API modes prevent mixed cache/decoder execution. Cache mode updates `last_instr_` the same way as decoder mode. |
| **R3** | CPUFeaturesAuditor must be satisfied. | Predecode audits at registration. Runtime cache mode does not repeat the auditor check. |
| **R4** | Trace, debugger, and custom visitors need the visitor chain. | Those features belong to decoder mode. |
| **R5** | BType and guarded-page checks are runtime state. | Cache mode keeps the imported check, gated by predecoded BTI-relevant flags. |
| **R6** | Self-modifying code would stale the cache. | V1 assumes immutable registered code and exposes no invalidation API. |
| **R7** | Multiple simulators share code. | Shared append-only cache, serialized registration, lock-free current-range hits. |
| **R8** | Cache growth is unbounded. | Embedder owns the budget. V1 has no cap, LRU, or flush. |
| **R9** | iOS no-JIT compliance. | Entries are ordinary data; leaves are existing compiled functions. No executable memory allocation. |
| **R10** | Cache mode must match decoder mode exactly. | ShadowRunner is the V1 oracle, comparing registers and memory writes per step. |
| **R11** | Indirect branches may target cold PCs. | V1 accepts a range lookup on the next step. No indirect-target predictor. |
| **R12** | Unallocated, feature-gated, and unimplemented words. | Unallocated or future feature-gated words become data-in-stream sentinels: the range registers `Ok`, normal branches may skip them, and executing the word aborts through `VisitUnallocated` with the PC. VIXL-known-but-unimplemented forms use an `unimplemented` sentinel leaf and abort on execution with form detail. |

Additional risks:

- ShadowRunner can be wrong. It has its own tests, including intentional
  divergence injection.
- Marker comments can decay. Review them as part of the code change.
- Leaf resolution must avoid importing a broad dependency edge from VIXL headers
  into public gaby-vm headers.

## 8. Non-goals

V1 does not include:

- `FlushCodeRange` or cache invalidation;
- self-modifying code support;
- operand pre-extraction;
- basic-block linking;
- direct threading;
- SVE Z/P/FFR comparison in ShadowRunner;
- direct exclusive-monitor state comparison;
- hard performance-number acceptance;
- per-instance cache fallback;
- cache memory cap or LRU;
- runtime cache/decoder switching within one run.

## 9. Further Reading

- `gaby-vm-modification-sketch.md`: older high-level direction.
- [`vixl-fetch-decode-dispatch-deep-dive.md`](./vixl-fetch-decode-dispatch-deep-dive.md):
  dispatch cost breakdown and original R1-R12 list.
- [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md):
  factual file/line reference for decoder dispatch.
- [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md):
  simulator subsystem overview.
- [`vixl-extraction-map.md`](./vixl-extraction-map.md):
  imported-file tier list.
- [`../architecture.md`](../architecture.md):
  project memory model, threading model, and import boundary.

## Appendix: Measured Dispatch Numbers

Append cache hot-path measurements here when a change alters dispatch.
Single-run numbers are qualitative and should not be read as a stable benchmark
contract.

### A.1 Baseline: `predecode-cache-benchmark`

| workload | mode | iters/sec | ns/insn | cache/decoder |
|----------|------|----------:|--------:|--------------:|
| mixed | decoder | 68 | - | - |
| mixed | cache | 272 | - | ~4.0x |
| smoke | decoder | 221k | - | - |
| smoke | cache | 3.3M | - | ~14.6x |

Source: archived `predecode-cache-benchmark` proposal text.

### A.2 `predecode-cache-hotpath-speedup`

Run commands:

```sh
bench/bench_baseline --mode <decoder|cache> --seconds 1.0
bench/bench_smoke --mode <decoder|cache> --seconds 0.2
```

| workload | mode | iters/sec | ns/insn | cache/decoder |
|----------|------|----------:|--------:|--------------:|
| mixed | decoder | 69.14 | 223.74 | - |
| mixed | cache | 270.16 | 57.26 | ~3.9x |
| smoke | decoder | 204621 | 152.72 | - |
| smoke | cache | 3478372 | 8.98 | ~17x |

The smoke cache path improved most because dispatch overhead dominates that
workload. Mixed stayed around the earlier ratio because NEON and memory leaves
dominate its per-instruction cost.
