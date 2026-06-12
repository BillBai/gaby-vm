Ordering rule (TDD): write the failing test first, watch it fail for the right
reason, then make it pass. Keep `ctest -R vixl_port` green before and after the
hot-path edit. See [design.md](./design.md) and the
[ref doc](../../../docs/refs/gaby-vm-predecode-cache-data-in-stream-2026-06-10.md).

## 1. Baseline (green before)

- [x] 1.1 Build `build/debug` and run the guard rail: `ctest --test-dir build/debug -R vixl_port` is green. Record the current per-family ran/skipped counts (they must match `kFamilyBaselines`).

## 2. RED — failing cache unit test

- [x] 2.1 Add `test/predecode_cache_data_in_stream_test.cc` (gaby public API only, dual-track, in the style of `test/simulator_correctness.cc`). Construct a range `[real insns that branch over the data][unallocated data word(s)][LDR-literal target + real insns][ret/br xzr]` with a hand-encoded word verified to be unallocated.
- [x] 2.2 Assert: `RegisterCodeRange` returns `Ok`; the cache track runs the body, branches over the data, and the `LDR`-literal reads the embedded data bytes; cache track == debug track.
- [x] 2.3 Add a death-test sub-case (fork + check-for-signal, like `test/typed_register_io_abort_test.cc`): a range whose entry deliberately branches INTO the data word aborts on the cache track.
- [x] 2.4 Register the test in `test/CMakeLists.txt` (`add_executable` + `add_test`, `PRIVATE gaby_vm::gaby_vm`, `gaby_vm_apply_compile_flags`).
- [x] 2.5 Verify RED: the `Ok` assertion fails today with `InvalidArgument` ("range contains an unallocated instruction encoding") — proving both that the chosen word is unallocated and that the current code rejects it. If it does not reject, pick a different unallocated word.

## 3. GREEN — predecode_cache.cc change

- [x] 3.1 Add a `DataInStreamSentinelLeaf()` helper next to `UnimplementedSentinelLeaf()`, pointing at `&vixl::aarch64::Simulator::VisitUnallocated`, with a comment explaining it covers both unallocated and (forward-looking) feature-gated words.
- [x] 3.2 In the predecode loop, replace the `unallocated()` reject branch and the `!auditor_.InstructionIsAvailable()` reject branch with a single branch that writes a data sentinel entry (`form_hash=0`, `flags=0`, `DataInStreamSentinelLeaf()`) and `continue`s.
- [x] 3.3 Remove code orphaned by the edit (the `<sstream>` include, the `ostringstream`/`GetInstructionFeatures()` use of the old `UnsupportedFeature` branch); keep the `RegistrationError` struct and `SetError` (still used by size/overlap/OOM).
- [x] 3.4 Verify GREEN: the new test passes; `ReadLints` clean on the edited files.

## 4. Rebaseline the guard rail

- [x] 4.1 Rebuild `build/debug`; run `VIXL_PORT_REBASELINE=1 ctest --test-dir build/debug -R vixl_port` and read off the observed ran/skipped per family.
- [x] 4.2 Repeat for `build/release` (`VIXL_PORT_REBASELINE=1`).
- [x] 4.3 Update `kFamilyBaselines` in `test/test_support/vixl_asm/harness/gaby_two_track_main.h` with the measured debug/release pairs (expect integer/fp to gain the revived cases; confirm by measurement).

## 5. Reconcile the reversed design decision

- [x] 5.1 Update design.md R12 (`docs/refs/gaby-vm-predecode-cache-design.md`): unallocated/feature-gated → data sentinel (registers Ok, traps on execute), unifying with unimplemented handling; record the data-in-stream rationale.
- [x] 5.2 Update the §4.3.2 all-or-nothing / `UnsupportedFeature` rationale note (`...design.md:299`): it was about feature probing; the cache no longer rejects on per-word decode properties.

## 6. Final verification

- [x] 6.1 `ctest --test-dir build/debug` and `ctest --test-dir build/release` fully green (new test + `vixl_port` + the rest).
- [x] 6.2 `openspec validate predecode-cache-data-in-stream --strict` passes.
- [x] 6.3 Confirm the 5 previously-skipped cases (`ldr_literal`, `ldr_literal_custom`, `ldr_literal_custom_shared`, `ldr_literal_range`, `fjcvtzs`) now run and pass both oracles (not merely stop being skipped).
