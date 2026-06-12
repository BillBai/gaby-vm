# Tasks

Ordering rule: `ctest -R vixl_port` stays green at every step; the extraction tool is deleted
**last**. Each group ends with an explicit verification. See
[design.md](./design.md) and [the design doc §6](../../../docs/refs/gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md)
for the file groups (A/T/U/S) and marker text.

## 1. Pin the import SHA

- [x] 1.1 Confirm `../vixl` is at SHA `160c445` (`git -C ../vixl rev-parse HEAD`); if not, `git -C ../vixl checkout 160c445` before copying anything.
- [x] 1.2 Record the pinned SHA `160c445` in `docs/refs/vixl-extraction-map.md` next to the Tier-0 section (the anti-silent-ODR pin). Verify the file references the SHA.

## 2. Land the island (compile only, not yet wired to the harness)

- [x] 2.1 Create `test/test_support/vixl_asm/` with a `.clang-format` containing `DisableFormat: true`.
- [x] 2.2 Copy the A-group assembler files from the pinned `../vixl` (assembler / macro-assembler / SVE `.cc` / `assembler-base-vixl.h` / `code-buffer-vixl.{h,cc}` / `code-generation-scopes-vixl.h` / `macro-assembler-interface.h` / `invalset-vixl.h`); preserve each upstream license header; add the test-only marker; strip the no-guard self-include where present.
- [x] 2.3 Copy the T-group test-infra files (`test-utils-aarch64.{h,cc}`, `test-simulator-inputs-aarch64.h`, `test-utils.h`, `test-runner.{h,cc}`); same header/marker/strip rules. Do NOT copy `test/test-utils.cc`.
- [x] 2.4 Copy the U-group upstream bodies (`test-assembler-aarch64.cc`, `-fp-aarch64.cc`, `-neon-aarch64.cc`); same rules; drop their no-guard self-include.
- [x] 2.5 Write the S-group `test-utils-stub.cc` (gaby-authored, no upstream header) with `ExecuteMemory` as a no-op / abort-on-call; leave it minimal — symbols are filled in by the linker in §3.
- [x] 2.6 Add the `gaby_vm_vixl_asm_testonly` CMake target: gated on `GABY_VM_BUILD_TESTS`, include path with `Sources/gaby_vm/src` FIRST, PRIVATE link `gaby_vm::gaby_vm`, PRIVATE `VIXL_CODE_BUFFER_MALLOC` (+ the existing `VIXL_INCLUDE_TARGET_A64` / `VIXL_INCLUDE_SIMULATOR_AARCH64` / `$<$<CONFIG:Debug>:VIXL_DEBUG>`), no `::` alias. Do NOT touch `Sources/gaby_vm/src/CMakeLists.txt`.
- [x] 2.7 Verify: `Sources/gaby_vm/src/CMakeLists.txt` is byte-unchanged (`git diff`), the island target compiles, and the existing frozen `vixl_port` suite is untouched and still green (`ctest -R vixl_port`).

## 3. ODR / link smoke gate (hard gate before any harness work)

- [x] 3.1 Build a tiny smoke binary that links the island + `gaby_vm.a`, assembles `Mov(x0,1)` then `FinalizeCode`, and runs one upstream body on a VIXL reference `Simulator` (proves the `macro-assembler-aarch64.h:41 → gaby simulator` coupling compiles and links).
- [x] 3.2 Verify with `nm`: zero duplicate shared `vixl::` symbols, zero undefined SVE symbols, and the binary does not link `sys/mman` (`git grep sys/mman test/test_support/vixl_asm/` is empty). Iterate `test-utils-stub.cc` until undefined symbols resolve (expected: `ExecuteMemory` only). NOTE: closing the symbol gap surfaced 4 assembler-only `Instruction` setters that gaby's read-only `instructions-aarch64.cc` gates out (`#if 0`); the island re-supplies them via `src/aarch64/instruction-assembler-setters-aarch64.cc` (no duplicate — gaby_vm.a omits exactly those four). NOTE 2: `git grep` is a false pass on the still-untracked island — re-verified with `grep -rn`. That surfaced live `#include <sys/mman.h>` in the upstream test bodies (used only by simulated-Mmap/MTE tests); removed in §5 (the spec scenario was refined to grep the include directive, since explanatory comments legitimately name the header).

## 4. Build the two-track harness + oracle (standalone first)

- [x] 4.1 Write `vixl_port_oracle.{h,cc}` porting `SeatEntry` / `CheckAssert` / `DifferentialEqual` from `test/vixl_port/vixl_port_runner.cc` (preserve the absolute per-assertion checks and the full-`RegisterFile` differential compare). DONE: `harness/vixl_port_oracle.{h,cc}` (gaby-public-API only; reads RegisterFile snapshots).
- [x] 4.2 Add the one-time startup assertion that the gaby seeded entry state is field-equivalent to VIXL `Simulator::ResetState()` (`simulator-aarch64.cc:764`). DONE: `EntrySeedingEquivalent` + `AssertEntryEquivalentOnce` (excludes harness-owned sp/x30).
- [x] 4.3 Write `gaby_two_track_macros.h` (redefine `SETUP/START/END/RUN/ASSERT_EQUAL_*`; `END()` forces the literal pool inline + `core.Dump` anchor + `Br(xzr)`; `RUN()` runs both gaby tracks now, not capture-frozen) and the per-family `main()` with the `sigsetjmp + alarm` crash/hang guard ported from `tools/vixl_test_extract/extract_main.cc`, plus the per-case instruction cap. Carry over the feature deny-list and by-name isolation; do NOT port `IsNonPortableInstr`. DONE: macros header + `gaby_two_track_main.h`. Terminator is a raw `br xzr` via `ExactAssemblyScope` (MacroAssembler::Br(xzr) asserts under VIXL_DEBUG); the per-case cap is the reference-sim instruction cap (gates the unbounded gaby RunFrom).
- [x] 4.4 Verify: the oracle + macros + a trivial inline body compile and link against the island. DONE: `gaby_vm_vixl_asm_harness_smoke` passes (both tracks, both oracles).

## 5. Prove the integer family side-by-side

- [x] 5.1 Add `vixl_port_integer_live` (temporary name) that `#include`s the upstream `test-assembler-aarch64.cc` under the two-track macros; register it with CTest alongside the existing `vixl_port_integer`.
- [x] 5.2 Drive it green; triage any newly-surfaced unimplemented-leaf failures against the deny-list / by-name surface (a real gap is a triage item, not a silent skip). DONE. Triage outcomes: (a) all three engines now run the SAME slice at the SAME address (reference reads body-exit registers directly; no core.Dump) so ADR/literal results agree; (b) sp/LR unified to the gaby stack so push/pop and sp asserts agree; (c) `DisableGCSCheck` + reset of CPUFeatures/seen-features per case (the sims are reused); (d) RMW families (atomics/exclusives/CAS, NEON store-multiple ST1/2/3/4) run on BOTH tracks by resetting the body's stack frame between the three engine runs — `RUN()` passes the body's frame pointer and `TwoTrackRun` snapshots/restores the window `[its frame, the body frame)`; the per-case register-file snapshots are held in process-lifetime storage outside that window so the reset cannot wipe them; (e) configure_cpu_features* and branch_to_reg quarantined (feature-config mechanism / BTI wild-PC). Remaining skips (GCS/BTI aborts, ADRP `AllowPageOffsetDependentCode`, address-dependent literals) are legitimate, not gaby divergences.
- [x] 5.3 Verify: the live integer family is green AND its included-case count clearly exceeds the old 90 (the memory-access bodies that were structurally skipped now run). Confirm at least one LDR/STR body and one ADR/literal body pass. DONE: `vixl_port_integer_live` passes (CTest); 185 passed / 73 skipped / 0 FAILED at the time of this task. LDR/STR/LDP/STP bodies pass (ldr_str_offset, ldr_str_pre/postindex, ldp_stp_offset, ldur_stur); the ADR body passes; atomics/exclusives/CAS pass too (per-run memory reset). (Post-review-fix baseline, now authoritative: integer 188 passed / 70 skipped debug, 189 / 69 release — the guarded-pages reset revived 3 BTI/branch cases.)

## 6. Switch integer, then repeat for fp and neon

- [x] 6.1 Switch integer: delete `test/vixl_port/generated/integer_fixtures.inc` + `vixl_port_integer.cc`, rename `_live` back to the canonical `vixl_port_integer`. Verify `ctest -R vixl_port` green.
- [x] 6.2 Repeat §5 + §6.1 for the fp family (`test-assembler-fp-aarch64.cc`). Verify green. (fp: 64 passed / 12 skipped / 0 FAILED at the time of this task; post-review-fix authoritative: 73 passed / 3 skipped after enabling the FP16 absolute oracle.)
- [x] 6.3 Repeat §5 + §6.1 for the neon family (`test-assembler-neon-aarch64.cc`). Verify green. SVE stays skipped (assembled symbols satisfied, bodies not run). (neon: 236 passed / 19 skipped / 0 FAILED at the time of this task; the NEON store-multiple ST1/2/3/4 bodies run on both tracks via the per-run memory reset.) All three canonical families now live-assemble; `ctest -R vixl_port` green under both presets. (Post-review-fix baseline, now authoritative: neon 254 passed / 1 skipped — V-vs-X bank-aware reads revived 3 `ASSERT_EQUAL_64(<bits>, dN)` cases, and enabling the FP16 absolute oracle revived 15 more.)

## 7. Remove the frozen-fixture model

- [x] 7.1 Delete `test/vixl_port/vixl_port_fixture.h`, `vixl_port_runner.{h,cc}`, the whole `test/vixl_port/generated/` tree (`.inc` + manifests), and any now-dead `gaby_vm_add_vixl_port_test` plumbing. DONE: removed the entire `test/vixl_port/` directory and the `add_subdirectory(vixl_port)` from `test/CMakeLists.txt`.
- [x] 7.2 Verify: `git grep` finds no reference to the deleted files; `ctest -R vixl_port` green under dev-debug and dev-release. DONE: no build reference remains (only provenance comments + the soon-deleted extraction tool's string literal); `ctest -R vixl_port` passes under both presets. (The `164/64/215` figures here were a stale mid-migration snapshot; the authoritative post-review-fix passed/family counts are integer 188 (debug) / 189 (release), fp 73, neon 254, all 0 FAILED — pinned in `kFamilyBaselines`.)

## 8. Remove the extraction tool (last, reversible until here)

- [x] 8.1 Remove `tools/vixl_test_extract/` and its `add_subdirectory`; remove the `GABY_VM_BUILD_VIXL_EXTRACT` option from the top-level `CMakeLists.txt` (salvage anything still needed — the crash guard and duck-typed assertion recorder — into the new harness first). DONE: the crash/hang guard is in `gaby_two_track_main.h` and the duck-typed recorders in `gaby_two_track_macros.h`; the whole `tools/` dir and both CMake hooks are removed.
- [x] 8.2 Verify: clean configure + build + full `ctest` for both dev-debug and dev-release with no dangling reference to `GABY_VM_BUILD_VIXL_EXTRACT` or the tool. DONE: clean `build/debug` and `build/release` both configure + build + `ctest` 17/17 PASS; `git grep` finds no `GABY_VM_BUILD_VIXL_EXTRACT` / `vixl_test_extract` outside historical docs.

## 9. Update the boundary docs (upstream license headers untouched)

- [x] 9.1 `docs/refs/vixl-extraction-map.md`: note Tier-0 now has a test-only copy under `test/test_support/vixl_asm/` at pinned SHA `160c445`, excluded from `gaby_vm`.
- [x] 9.2 `docs/architecture.md`: distinguish "shipping import Tiers 1-3" from "test-only Tier-0 copy" in the VIXL import boundary section.
- [x] 9.3 `docs/testing.md`: rewrite the `vixl_port` section — live-assemble + two-track run, real memory-access coverage, no frozen fixtures, no extraction tool; remove the "ported suite covers no memory-access semantics" statement. (Also adjusted the Encoding-policy note for the test-only island exception.)
- [x] 9.4 `AGENTS.md`: delete the "regenerate the committed fixtures … `GABY_VM_BUILD_VIXL_EXTRACT`" sentence from the guard-rail bullet; keep the rest of the bullet. (Also updated "replays" → "live-assembles".)

## 10. Final verification

- [x] 10.1 Full `ctest` green under dev-debug and dev-release; `ctest -R vixl_port` enumerates the live families. DONE: clean full `ctest` 17/17 under both presets; `ctest -N -R vixl_port` lists exactly `vixl_port_integer` / `_fp` / `_neon`.
- [x] 10.2 `openspec validate vixl-port-live-assemble-rewrite --strict` passes; spec scenarios (island non-linkage, MALLOC-only, no `sys/mman`, memory-access body green) are each satisfied. DONE: validate reports valid; island TUs compile `VIXL_CODE_BUFFER_MALLOC` (×10) and never `VIXL_CODE_BUFFER_MMAP` (0 project-wide); no shipping source carries a `VIXL_CODE_BUFFER_*` define; no live `#include <sys/mman.h>` in the island; no `::` alias; the 8 shared leaf headers are not re-copied; a load+store body (`ldr_str_offset`) passes on both tracks.
- [x] 10.3 Confirm `gaby_vm::gaby_vm` link inputs contain no island object and `Sources/gaby_vm/src/CMakeLists.txt` is byte-unchanged. DONE: `nm` shows 0 `vixl::aarch64::Assembler::` definitions in `libgaby_vm.a` (the assembler is island-only); `git diff` on `Sources/gaby_vm/src/CMakeLists.txt` is empty.
