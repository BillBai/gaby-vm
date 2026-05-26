> Prerequisite: `predecode-cache-core` (archived 2026-05-25) and
> `predecode-cache-benchmark` (archived 2026-05-26) are both in the live
> spec set. This change consumes their internals (`FormToVisitorFnMap`,
> `PredecodedEntry`, the predecode pass) and their measurement
> instrument (`bench/runner` cache engine).

## 1. Leaf dispatch: `std::function` → raw pmf

- [ ] 1.1 In `src/aarch64/simulator-aarch64.h`, change `FormToVisitorFnMap`'s value type from `std::function<void(Simulator*, const Instruction*)>` to `void (Simulator::*)(const Instruction*)`. Add a `static_assert` on `sizeof(FormToVisitorFnMap::mapped_type)` next to the type alias as the ABI-drift tripwire (D1, Risks).
- [ ] 1.2 In `src/aarch64/simulator-aarch64.cc`, confirm every entry of the `form_to_visitor` initializer is `&Simulator::SimulateXxx` (i.e., a pmf with the new signature). Mass-rebuild and fix any entry whose previous `std::function` indirection masked a signature mismatch.
- [ ] 1.3 In `Simulator::ResolvePredecodeLeaf` (`simulator-aarch64.h`), keep the `const void*` return type — only the underlying pointee changes. Update the surrounding gaby-vm marker comment to name the new pmf pointee type.
- [ ] 1.4 In `Simulator::ExecuteInstructionCached` (`simulator-aarch64.h`), update the dispatch line to cast `entry->leaf` back to `const FormToVisitorFnMap::mapped_type*`, dereference, and call as a pmf: `(this->*pmf)(pc_)`. Leave the `form_hash_` store untouched.
- [ ] 1.5 Confirm `predecode_cache.cc`'s populate pass needs no edits — it stores `ResolvePredecodeLeaf`'s opaque result verbatim and does not inspect the pointee. Grep `FormToVisitorFnMap` to verify no other callsite exists outside `simulator-aarch64.{h,cc}`.

## 2. BTI relevance: predecode-time classification

- [ ] 2.1 In `src/gaby_vm/predecode_cache.cc`, add a helper that classifies a given `(form_hash, raw instruction word)` pair as BTI-relevant. The classification fires for `BTI`, `PACIASP`, `PACIBSP`, `BRK`, `HLT`, and exception-causing forms (`SVC`, `HVC`, `SMC`, `UDF`/unallocated, plus anything else the imported simulator's hot-path branch examined). Use VIXL's existing `Instruction` accessors (`IsBti()`, `IsPAuth()`, `IsException()`) to mirror the current runtime check exactly.
- [ ] 2.2 In the predecode pass's per-instruction loop, write bit 0 of `PredecodedEntry::reserved` from the classification. Document the bit's meaning at the field's declaration in `include/gaby_vm/predecode_cache.h` — keep the field a private allocation but make the documentation honest. (No public API surface change; only the comment.)
- [ ] 2.3 In `Simulator::ExecuteInstructionCached` (`simulator-aarch64.h`), wrap the existing `if (PcIsInGuardedPage() && (ReadBType() != DefaultBType)) { ... }` block in an outer `if ((entry->reserved & 1u) != 0u) { ... }`. Inside the new wrapper, the body is byte-for-byte the existing block. Add a gaby-vm marker comment explaining the gating.

## 3. Correctness regression: dual-track BTI sub-test

- [ ] 3.1 In `test/simulator_correctness.cc`, add a hand-encoded sub-test under the existing control-flow family that exercises a `PACIASP` + `BTI j` + `RET` sequence. The expectation is that both tracks complete without abort and produce identical state. (No guarded-page setup — current builds don't model it; the test's purpose is to cover the BTI-relevant classification on the cache track.)
- [ ] 3.2 Confirm `workload_shadow_test` reports zero divergence on `smoke` and `mixed` after the cache hot-path change. This is the V1 oracle; any silent regression introduced by the classification surfaces here.

## 4. Measurement: record the new numbers

- [ ] 4.1 Build the bench targets under the `dev-release` preset with `GABY_VM_BUILD_BENCHMARKS=ON`.
- [ ] 4.2 Run `bench_baseline --engine decoder --seconds 1.0`, `bench_baseline --engine cache --seconds 1.0`, `bench_smoke --engine decoder --seconds 0.2`, and `bench_smoke --engine cache --seconds 0.2`. Capture each invocation's full key/value output.
- [ ] 4.3 Append the new numbers and their cache-vs-decoder ratios to `docs/refs/gaby-vm-predecode-cache-design.md`'s measurement appendix, alongside the predecode-cache-benchmark baseline (`~4×` / `~14.6×`). No committed `N×`; just record the delta the change actually produced.

## 5. Acceptance verification

- [ ] 5.1 `ctest --preset dev-debug` and `ctest --preset dev-release` both pass on every existing case (`workload_shadow`, `simulator_correctness`, `reentrancy`, `shadow_runner`, `typed_register_io`, `typed_register_io_abort`, `simulator_constructor_stack`).
- [ ] 5.2 `git diff --name-only` touches only paths under `src/`, `include/gaby_vm/predecode_cache.h` (comment only), `test/simulator_correctness.cc`, `docs/refs/gaby-vm-predecode-cache-design.md`, and the change's own `openspec/changes/predecode-cache-hotpath-speedup/` directory. Nothing under `bench/`, no imported VIXL file outside marker blocks.
- [ ] 5.3 `openspec validate predecode-cache-hotpath-speedup --strict` returns `valid`. Then `openspec archive predecode-cache-hotpath-speedup` to apply the `predecode-cache` delta to the live spec.
