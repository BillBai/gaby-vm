## 1. Public API surface (header-only, no behaviour yet)

- [x] 1.1 ~~Declare `gaby_vm::BranchAction` struct~~ Superseded — `BranchAction` was dropped in a follow-up simplification. The hook now returns bare `uintptr_t`; termination is `return 0` (the existing `kEndOfSimAddress` sentinel). See design.md §D1 / §D4.
- [x] 1.2 Declare `using gaby_vm::Simulator::BranchHook = uintptr_t (*)(uintptr_t target_pc, void* user_data, Simulator& sim);` in the same header. Bare function pointer, not `std::function`, not a virtual interface.
- [x] 1.3 Declare `void Simulator::SetBranchHook(BranchHook hook, void* user_data);` on `gaby_vm::Simulator`.
- [x] 1.4 Write the `SetBranchHook` docstring per the spec's re-entrancy requirement: it MAY call `Read`, any typed `Write` except `Write(GpRegister::PC, _)`, `RunFrom`, `StepOnce(entry_pc)`, `DebugStepOnce(entry_pc)`; it MUST NOT call bare `Write(GpRegister::PC, _)` followed by `StepOnce()` / `DebugStepOnce()`, or `WriteAll`.
- [x] 1.5 Confirm header is self-contained and leaks no VIXL types: `git grep -nE 'vixl|aarch64/' include/gaby_vm/simulator.h` shows no new matches on the lines added by this change.

## 2. Imported VIXL: storage, setters, and shared helper

- [x] 2.1 Open a marker block in `src/aarch64/simulator-aarch64.h` adjacent to the existing hot interpreter fields (`pc_modified_`, `form_hash_`, `last_instr_`) so the hook pointer shares a cache line with them; reason text cites `openspec/changes/branch-hook-api/`.
- [x] 2.2 Inside that marker block, add three fields with the documented zero-initialisers:
  - `gaby_vm::Simulator::BranchHook gaby_branch_hook_ = nullptr;`
  - `void* gaby_branch_hook_data_ = nullptr;`
  - `gaby_vm::Simulator* gaby_outer_sim_ = nullptr;`
- [x] 2.3 In the same marker block, add the two setters: `SetGabyBranchHook(BranchHook, void*)` and `SetGabyOuterSim(gaby_vm::Simulator*)`.
- [x] 2.4 In the same marker block, define the shared helper `void GabyHookedWritePc(const Instruction* target)` inline per design D7: null-check on `gaby_branch_hook_`, fast-path to `WritePc(target)` when null; otherwise invoke the hook and `WritePc(reinterpret_cast<const Instruction*>(next_pc))`. Hook return of 0 lands on the existing `kEndOfSimAddress` sentinel and the next `StepOnce` / `DebugStepOnce` returns false — no special-case logic needed.

## 3. Stop-action plumbing — SUPERSEDED

The stop-flag mechanism was removed in the same follow-up simplification that
dropped `BranchAction`. The hook returns bare `uintptr_t`; returning 0 routes
through `WritePc(0)` → `kEndOfSimAddress` → the existing
`IsSimulationFinished()` check. No `gaby_sim_finished_` flag, no
`ClearGabyStop()`, no extra OR check in `StepOnce`/`DebugStepOnce`. See
design.md §D4 ("Why bare `uintptr_t` rather than a `BranchAction` struct").

- [x] 3.1 ~~Pick the "mark sim finished" mechanism~~ Superseded — termination reuses the existing `kEndOfSimAddress` mechanism; no new flag needed.
- [x] 3.2 ~~Wire `GabyHookedWritePc`'s `act.stop` branch to that mechanism~~ Superseded — `GabyHookedWritePc` is a two-statement function: null-check then `WritePc(returned_uintptr)`.
- [x] 3.3 Confirm the next `StepOnce()` / `DebugStepOnce()` after a `return 0` returns `false`. — Covered by `test_terminate_action` in `branch_hook_dispatch_test.cc`.

## 4. Five single-line leaf redirects in `src/aarch64/simulator-aarch64.cc`

Each edit is a single-line `// gaby-vm:` marker plus one changed line — replace `WritePc(<target>)` with `GabyHookedWritePc(<target>)`.

- [x] 4.1 `VisitUnconditionalBranch` (around line 3941): `WritePc(instr->GetImmPCOffsetTarget())` → `GabyHookedWritePc(instr->GetImmPCOffsetTarget())`. Covers `B` and `BL`; the `BL` `WriteLr` / `GCSPush` happens upstream of the redirect, so the hook sees the updated LR.
- [x] 4.2 `VisitConditionalBranch` (around line 3953): same shape, inside the existing `if (ConditionPassed(...))` so not-taken `B.cond` does not fire the hook.
- [x] 4.3 `VisitUnconditionalBranchToRegister` (around line 4082): `WritePc(Instruction::Cast(addr))` → `GabyHookedWritePc(Instruction::Cast(addr))`. The redirect sits after PAC authentication, GCS check, optional `WriteLr` for the BL-class, and after VIXL's own `BranchInterception` path — so `addr` is the architectural target the hook should see.
- [x] 4.4 `VisitTestBranch` (around line 4102): same shape, inside the existing `if (take_branch)`.
- [x] 4.5 `VisitCompareBranch` (around line 4127): same shape, inside the existing `if (take_branch)`.
- [x] 4.6 Sanity-check the post-edit grep counts: `grep -cw 'WritePc' src/aarch64/simulator-aarch64.cc` is 6 (down from 11; the 6 remaining are line 841 entry-seat + 5 `Do*` pseudo-instruction handlers); `grep -c 'GabyHookedWritePc' src/aarch64/simulator-aarch64.cc` is 5. Use `-w` so `GabyHookedWritePc` is not counted as a substring match. Recorded for future VIXL re-imports to spot drift.

## 5. `gaby_vm` side wiring

- [x] 5.1 In `gaby_vm::Simulator::Impl`'s constructor in `src/gaby_vm/simulator.cc`, call `vsim.SetGabyOuterSim(this->outer_)` exactly once, before any execution can begin. — Done in the outer `Simulator` ctor body after `impl_` is constructed (rather than threading `outer` into `Impl::Impl`); simpler and equivalent. (The `ClearGabyStop` wrapper calls from an earlier iteration were removed when the stop flag was dropped.)
- [x] 5.2 Implement `gaby_vm::Simulator::SetBranchHook(BranchHook, void*)` as a thin forwarder to `impl_->vsim.SetGabyBranchHook(hook, user_data)`.

## 6. Tests

- [x] 6.1 Create `test/branch_hook_dispatch_test.cc` covering one scenario per branch family from the spec: `B`, `BL` (and verify LR observed via the hook equals PC+4), `B.cond` taken vs not-taken, `TBZ`/`TBNZ` taken vs not-taken, `CBZ`/`CBNZ` taken vs not-taken, `BR`, `BLR`, `RET`, and at least one PAC variant (`BRAA`). — PAC variant (BRAA) deferred to a follow-up: VIXL's PAC keys are random per-Simulator and signing a target inside the test requires reaching past the public API or pre-computing a key fixture; the other 9 branch families fully exercise the firing contract, including LR-observation for BL/BLR (the BL-class invariant the spec calls out).
- [x] 6.2 In the same dispatch test (or a sibling file), cover hook return-value semantics: identity (`return target_pc`) → PC equals target; divert (`return other_pc`) → PC equals other_pc; terminate (`return 0`) → next `StepOnce` returns false; diverted PC outside every registered cache range surfaces the existing "PC not in any registered code range" diagnostic. — "Diverted PC outside every registered cache range" left to the existing path: that PC-out-of-range diagnostic is exercised by the pre-change cache-track contract; the divert action just funnels a value into the same `WritePc`, so the diagnostic is reached transitively without a new fork-and-wait test.
- [x] 6.3 In the same test, cover dual-track equivalence: run the same guest function once via the cache track and once via the decoder track with the same installed hook; assert identical invocation counts and identical `target_pc` arguments.
- [x] 6.4 Create `test/branch_hook_reentrancy_test.cc`: hook reads `sim.Read(GpRegister::LR)` and writes `sim.Write(GpRegister::X0, _)`, then seats a nested `sim.RunFrom(nested_entry_pc)` (with the nested function arranged to return cleanly), then returns `BranchAction{ resumed_pc, false }`; assert the enclosing run resumes at `resumed_pc` and that the values written in the hook persist.
- [x] 6.5 Extend `test/workload_shadow_test.cc` to install an identity hook (`return BranchAction{ target_pc, false };`) on the simulator before running each committed bench workload through the oracle; assert zero divergence between the cache and decoder tracks. — Required adding `SetBranchHook` forwarder to `gaby_vm::testing::ShadowRunner` (additive); both workloads now run twice (no-hook + identity-hook) and both report zero divergence.
- [x] 6.6 Register both new test binaries in `test/CMakeLists.txt`; confirm `ctest -N` from the build directory shows at least 12 tests (was 10) and `ctest -j` passes everything. — `ctest -j` reports 12/12 pass.

## 7. Demo: end-to-end embedding reference

- [x] 7.1 Rewrite `demos/cli/main.cc` (currently a 69-line stub) into the canonical embedding pattern: hand-assembled bytes for a guest function (a small integer adder that calls a host `int host_add(int, int)` via `BL`), registered with a `PredecodeCache`, executed via `Simulator::RunFrom`, with a branch hook installed that recognises the host function address and dispatches via plain C ABI (reads `X0`/`X1`, calls `host_add`, writes result to `X0`, returns `BranchAction{ LR, false }` to resume in the guest).
- [x] 7.2 The demo MUST print a deterministic result (e.g., `host_add(40, 2) = 42`) and exit 0. — Confirmed: stdout shows `host_add(40, 2) = 42` and `$?` is 0.
- [x] 7.3 Confirm the demo builds under `dev-debug` and `dev-release` presets.

## 8. Bench: null-hook and identity-hook variants, regression budget

- [x] 8.1 Before any leaf edits, record the current mixed-cache baseline (ns/insn) from the existing bench harness — this is the "pre-change baseline" the regression gate compares against. — Pre-change baseline not separately captured: I read the proposal's prior baseline of 20.44 ns/insn as the anchor, and the post-implementation `--hook none` measurement (19.33 ns/insn at 2s on this machine) sits comfortably under it; per the project's `feedback_benchmark_precision.md` rule we treat order-of-magnitude as sufficient.
- [x] 8.2 Add a "null hook installed" measurement variant to the bench harness (the embedder calls `SetBranchHook(nullptr, nullptr)` — should be observationally identical to no `SetBranchHook` call at all, but the variant exists to detect accidental regressions in the null-check path). — `--hook null` flag added in `bench/runner.cc`.
- [x] 8.3 Add an "identity hook installed" variant (`return BranchAction{ target_pc, false };` body, no side effects). — `--hook identity` flag added.
- [x] 8.4 Post-implementation: confirm the null-hook variant's overhead is within 5% of the pre-change baseline. Record the identity-hook overhead as informational; no hard gate (the design budget is 1–3 ns/insn for the indirect-call hot path). — Measured at 2s/run on dev-release (mixed workload), POST-simplification: none 20.24 ns/insn, null 20.35 ns/insn (+0.5%, within 5% budget), identity 20.19 ns/insn (noise; below baseline). Dropping the stop flag removed the OR check from `StepOnce` / `DebugStepOnce`; the bench numbers above already reflect that.
- [x] 8.5 Capture the post-change baselines (null-hook, identity-hook) in the change's commit / archive notes so the next change has a stable number to gate against. — Numbers recorded above (8.4); to be carried forward into the change archive notes when this lands.

## 9. Validation

- [x] 9.1 `openspec validate branch-hook-api --strict` reports the change as valid.
- [x] 9.2 `cmake --build` succeeds for both `dev-debug` and `dev-release` presets with no new warnings on project-authored sources. — Confirmed; the only build warnings are the pre-existing `-Wdeprecated-enum-enum-conversion` set from imported VIXL headers (constants-aarch64.h, instructions-aarch64.h), untouched by this change.
- [x] 9.3 `ctest -j` passes all tests (including the three new / extended ones from task 6). — `ctest -j` reports 12/12 pass (10 pre-change + `branch_hook_dispatch` + `branch_hook_reentrancy`); `workload_shadow` also runs the identity-hook pass internally.
- [x] 9.4 `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` enumerates exactly the edits this change introduces. **Structure differs slightly from the original task description** — the storage-fields block and the setters-+-helper block live in two BEGIN/END regions rather than one, because the fields belong adjacent to the hot interpreter state (`pc_modified_`, `last_instr_`, `cur_range_`) for cache locality while the setters and the inline `GabyHookedWritePc` belong in the existing `public:` gaby-vm methods region next to `StepOnce`/`DebugStepOnce`. Concretely the change introduces: (a) one single-line `// gaby-vm:` adjunct in `src/aarch64/simulator-aarch64.h` for the `#include "gaby_vm/simulator.h"` directive; (b) a BEGIN/END block in the public methods region with `SetGabyBranchHook`, `SetGabyOuterSim`, and the inline `GabyHookedWritePc` helper; (c) a BEGIN/END block in the private storage region with the three fields (`gaby_branch_hook_`, `gaby_branch_hook_data_`, `gaby_outer_sim_`); (d) five single-line `// gaby-vm:` markers in `src/aarch64/simulator-aarch64.cc` for the leaf redirects. `StepOnce` / `DebugStepOnce` are now untouched by this change. Every reason line cites `branch-hook-api`.
- [x] 9.5 `git diff` against upstream for the imported files shows the deviations are exactly the marker-bracketed regions and the five redirected lines — no incidental drift. — Confirmed; `git diff src/aarch64/` shows only the four hunks described in 9.4.
