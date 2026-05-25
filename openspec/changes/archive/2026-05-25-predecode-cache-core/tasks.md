> Implementation order follows the design doc's dependency chain. Each group
> ends at a buildable, test-green checkpoint. File/line anchors and rationale
> live in `docs/refs/gaby-vm-predecode-cache-design.md` (cited as "design doc
> §X"); this list says *what* to do and *how to know it is done*.

## 1. Scaffold the `gaby_vm` cache library surface

- [x] 1.1 Create `include/gaby_vm/predecode_cache.h`: Pimpl `PredecodeCache` (ctor, dtor, deleted copy, `RegisterCodeRange`, `GetLastErrorDetail`), the `RegisterStatus` `enum class : int`, the `ErrorDetail` POD, and `PredecodedEntry` (the 16-byte struct — form hash + leaf function pointer — exposed for inline lookup; design.md D8). No `vixl::*` symbol, no imported VIXL include.
- [x] 1.2 Create `include/gaby_vm/simulator.h`: Pimpl `gaby_vm::Simulator` — ctor `(PredecodeCache*, void* stack_buf, size_t stack_size)`, dtor, `RunFrom` / `DebugRunFrom` / `StepOnce` / `DebugStepOnce`, register/PC accessors, trace/visitor setters. No `vixl::*` symbol.
- [x] 1.3 Create `include/gaby_vm/shadow_runner.h`: `namespace gaby_vm::testing` with `ShadowRunner`, `DivergenceReport`, and the `DivergenceHandler` callback type. No `vixl::*` symbol.
- [x] 1.4 Create `src/gaby_vm/` with stub `predecode_cache.cc`, `simulator.cc`, `shadow_runner.cc`; add the sources to the `gaby_vm::gaby_vm` library build. Configure + build is green with the stubs.
- [x] 1.5 Confirm `git grep -n 'vixl::' include/gaby_vm/` is empty and no `include/gaby_vm/` header includes an imported VIXL header (public-header hygiene; existing `aarch64-simulator` rule).

## 2. Imported-file hooks — the first `// gaby-vm` markers

- [x] 2.1 In `src/aarch64/simulator-aarch64.h`, add a marker-bracketed `MemoryWriteSink` interface class, a `write_sink_` field defaulting to `nullptr`, and `SetMemoryWriteSink`. Marker reason cites design doc §4.4.2 / §5.7.
- [x] 2.2 Add the marker-bracketed write hook in the `Simulator::MemWrite<T>` path — `if (VIXL_UNLIKELY(write_sink_)) write_sink_->Record(...)` — recording 128-bit writes as lo/hi halves. Upstream memcpy/atomic logic is untouched.
- [x] 2.3 Add the marker-bracketed `cache_` and `cur_range_` fields to imported `Simulator`, plus the marker-bracketed `#include "gaby_vm/predecode_cache.h"` needed for `PredecodedEntry`.
- [x] 2.4 Add the marker-bracketed `ExecuteInstructionCached`, `StepOnce`, and `DebugStepOnce` members. `ExecuteInstructionCached` keeps the pre/post steps of `ExecuteInstruction` (BType, guarded-page, `IncrementPc`, `UpdateBType`, `last_instr_`) and skips only the auditor `VIXL_CHECK` (design doc §3, §6.1, §6.5).
- [x] 2.5 Resolve OQ1 from `design.md`: pick how `PredecodeCache` reaches the `form_hash → leaf_fn` map (direct `GetFormToVisitorFnMap` access vs. a capture visitor). (OQ2 is closed — D8 drops the thunk, so `ExecuteInstructionCached` writes `form_hash_` as an ordinary member.)
- [x] 2.6 Confirm `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` enumerates every new region and each marker reason names `docs/refs/gaby-vm-predecode-cache-design.md`. Build green.

## 3. Predecode cache core

- [x] 3.1 Implement `CodeRange` and the `start`-sorted range table in `PredecodeCache::Impl`. Hold `CodeRange` records in stable storage (never relocated on append) so a `cur_range_` pointer cannot dangle, and guard the table with a `std::shared_mutex` — `RegisterCodeRange` exclusive, slow-path search shared (design.md D9).
- [x] 3.2 Define the 16-byte `PredecodedEntry` — the instruction's `form_hash` plus its leaf member-function pointer — and the populate-time resolution of that pair for a decoded instruction. V1 stores the pair directly; the per-form thunk is deferred to V2 (design.md D8).
- [x] 3.3 Implement `RegisterCodeRange`: allocate the `PredecodedEntry` array, run the predecode pass (decode each word, record its form hash + leaf function pointer into the entry), and pre-screen every instruction with `CPUFeaturesAuditor`.
- [x] 3.4 Implement validation and error reporting: `OverlappingRange`, `UnsupportedFeature` + populated `ErrorDetail`, `InvalidArgument`, `OutOfMemory`; failed calls register nothing (all-or-nothing; design doc §4.3.2–§4.3.3).
- [x] 3.5 Implement PC→entry lookup: the per-`Simulator` `cur_range_` fast path (lock-free) plus a binary-search slow path that takes the table's shared lock and updates `cur_range_` (design doc §4.2.3, design.md D9).
- [x] 3.6 Handle unallocated / unimplemented encodings (design doc R12): unallocated → registration fails with `ErrorDetail`; unimplemented → a sentinel leaf entry that aborts naming the form.

## 4. `gaby_vm::Simulator` (Pimpl)

- [x] 4.1 Implement `Simulator::Impl` holding a `vixl::aarch64::Simulator`, a `Decoder` for the debug track, and the nullable `PredecodeCache*`.
- [x] 4.2 Implement the cache track (`RunFrom`, `StepOnce`) over `ExecuteInstructionCached`: an out-of-range PC aborts with a diagnostic naming the address; a cache-track call on a null-cache `Simulator` aborts.
- [x] 4.3 Implement the debug track (`DebugRunFrom`, `DebugStepOnce`) over the imported `Decoder → VisitNamedInstruction → leaf` flow; trace/debugger/visitor setters take effect on this track only.
- [x] 4.4 Implement register/PC accessors and the `RET`-to-NULL-`LR` termination convention (the `kEndOfSimAddress` sentinel) that the tests and benchmarks rely on.
- [x] 4.5 Make `RunFrom` / `DebugRunFrom` / `StepOnce` / `DebugStepOnce` re-entrant: snapshot the interpreter cursor (`pc_`, `cur_range_`, `form_hash_`, `last_instr_`, `pc_modified_`) on entry and restore it on return (RAII); the guest register file is not saved/restored (design.md D10). Add a test that nests `RunFrom` from within a leaf / bridge callback and asserts the enclosing run resumes intact.
- [x] 4.6 Checkpoint: run a `NOP; RET` sequence through both `RunFrom` and `DebugRunFrom`; both terminate cleanly.

## 5. ShadowRunner

- [x] 5.1 Implement per-track `MemoryWriteSink` subclasses that capture each step's `(addr, size, value)` write list.
- [x] 5.2 Implement `ShadowRunner` holding two `gaby_vm::Simulator`s — `fast_` (cache) and `ref_` (null cache) — over one shared stack buffer, with mirrored register writes (design doc §4.4).
- [x] 5.3 Implement lockstep `RunFrom`: alternate `StepOnce` / `DebugStepOnce` and, after each step, compare X0–X30, SP, V0–V31 (full 128-bit), PC, NZCV, FPCR, FPSR, BType, and the memory-write lists (design doc §4.4.1).
- [x] 5.4 Implement `DivergenceReport` construction and the `DivergenceHandler` hook; the default handler dumps the diff to stderr and aborts; a custom handler can be installed.

## 6. Tests

- [x] 6.1 Rework `test/simulator_correctness.cc` into the dual-path harness: each existing hand-encoded sequence (integer arithmetic, logical, load/store, conditional control flow, procedure call/return) runs through `gaby_vm::Simulator::RunFrom` and `DebugRunFrom`; assert both reach the precomputed expected state.
- [x] 6.2 Update `test/CMakeLists.txt`: `simulator_correctness` links `gaby_vm::gaby_vm` only — drop the privileged `PRIVATE ${PROJECT_SOURCE_DIR}/src` include and the `VIXL_*` defines — and keep `add_test(NAME simulator_correctness ...)`.
- [x] 6.3 Add `test/shadow_runner_test.cc` and register it with `add_test`: a matching workload reports zero divergence; a deliberately injected fast-path defect IS detected; the custom `DivergenceHandler` path works.
- [x] 6.4 Run `ctest` under the `dev-debug` and `dev-release` presets: `simulator_correctness`, `shadow_runner`, `simulator_smoke`, and `smoke` all pass.

## 7. Acceptance verification

- [x] 7.1 `simulator_correctness` is green across both tracks under `dev-debug` and `dev-release` (predecode-cache spec: *Dual-path correctness …*).
- [x] 7.2 `shadow_runner` is green, including the injected-defect sub-case (predecode-cache spec: *… the shadow oracle are registered with CTest*).
- [x] 7.3 Run `ShadowRunner` over every workload under `bench/workloads/` — zero divergence (design doc §4.5 acceptance #3). This consumes the committed workload data only; it does not need the benchmark harness.
- [x] 7.4 `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` enumerates every imported-file drift, and each marker reason names the design document (`aarch64-simulator` marker requirement).
- [x] 7.5 `git grep -n 'vixl::' include/gaby_vm/` is empty and no public header includes an imported VIXL header.
- [ ] 7.6 Run `openspec validate predecode-cache-core --strict` (expect `valid`), then `openspec archive predecode-cache-core` — folding the `predecode-cache` capability into a new live spec and applying the `aarch64-simulator` removal. (Speed acceptance — design doc §4.5 #1 — lands with the follow-up `predecode-cache-benchmark` change.)
