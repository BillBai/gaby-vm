# Tasks: cache-hotpath-tier1

Guard-rail loop for every implementation task (referred to as **GRL** below):
build dev-debug + dev-release → `ctest --test-dir build/debug -R vixl_port`
green → `bench_business --verify` clean → 3× `bench_business --mode cache`
per shape, record ns/insn in the numbers table (section 8). One commit per
task unless a task says otherwise. Baseline (M1 Pro, 2026-07-03): parse 9.34 /
hash 10.49 / struct 10.81 / fsm 9.34 / applogic 14.26.

## 1. Prerequisites

- [x] 1.1 Record the pre-change baseline: build current branch, run GRL
      measurement steps (no code change), commit the numbers table skeleton.
- [ ] 1.2 T6 gate: disassemble `AddWithCarry` / `AddSubHelper` in the
      dev-release build; record whether clang already sinks the NZCV
      computation when `set_flags` is false. Decision: implement T6 or mark
      it no-op (design D6).
- [ ] 1.3 T3 tests first (design D4): add hand-encoded MOVPRFX protocol unit
      tests — positive pair (cache == decoder, no abort) and negative pair
      (abort via `CanTakeSVEMovprfx` on both tracks) at VL=128. Tests must
      pass against the CURRENT per-step implementation before T3 lands.

## 2. T1 — LogicVRegister right-sizing (largest item, applogic target)

- [ ] 2.1 Shrink `LogicVRegister::saturated_` / `round_` to the VL=128 lane
      bound with `static_assert` tied to `kZRegMinSizeInBytes`; marker
      convention (design D1).
- [ ] 2.2 Add the runtime VL guard in `SetVectorLengthInBits` (reject VL >
      128 while the bound is in force) and the revisit note in
      `docs/refs/vixl-extraction-map.md` (design D2).
- [ ] 2.3 Bound `SimRegisterBase::Write`'s clear to the actual register size
      (precedent: `ClearTail`).
- [ ] 2.4 Hoist the per-call `unordered_map` in `SimulateFPRoundInt` /
      `SimulateFPRoundIntToSize` to static storage.
- [ ] 2.5 GRL; expect the big applogic move (~14.3 → ~10-11 on this host).

## 3. T2 — Load/store leaf de-layering

- [ ] 3.1 Gate the trace-preparation tail in `LoadStoreHelper` and
      `LoadStorePairHelper` on the trace mask; verify decoder-track
      trace-enabled output is unchanged.
- [ ] 3.2 Precompute `SimStack` guard-region bounds at construction.
- [ ] 3.3 GRL for 3.1+3.2 (one commit).
- [ ] 3.4 Separate commit (design D5): early-return `LocalMonitor::
      MaybeClear` when unarmed, with the marker comment naming the upstream
      LCG-sequence deviation. GRL.

## 4. T3 — MOVPRFX flag-gating (needs 1.3 green)

- [ ] 4.1 Predecode: classify the two MOVPRFX form hashes into `flags` bit 1
      in `predecode_cache.cc`; document the bit next to bit 0.
- [ ] 4.2 Cache track: replace the per-step `form_hash_` compare-pair with
      the predecoded bit via `prev_was_movprfx_` (design D3); add the member
      to the re-entrancy cursor save/restore set.
- [ ] 4.3 Confirm 1.3's negative test still aborts and the re-entrancy tests
      pass; GRL.

## 5. T4 — Dispatch-hub epilogue strip

- [ ] 5.1 Single trace-mask gate replacing the three `ShouldTrace*` tests on
      the cache track; conditional `UpdateBType` (idempotence-guarded).
- [ ] 5.2 Fold `kEndOfSimAddress` load to a null compare on the cache-track
      step path; move the abort-path `ostringstream` out of
      `ExecuteInstructionCached` into a cold noinline helper; add
      likely/unlikely hints on the hot-path branches.
- [ ] 5.3 GRL (one commit for 5.1+5.2).

## 6. T5 — Branch-interception probe flag

- [ ] 6.1 Add the "any interception registered" flag on `MetaDataDepot`,
      set/cleared by registration and `ResetState`; skip the per-BR/BLR map
      probe when clear.
- [ ] 6.2 Confirm the interception-registering vixl_port bodies still pass
      (they keep the flag-on path covered); GRL.

## 7. T6 — AddWithCarry flag-skip (only if 1.2 says go)

- [ ] 7.1 Fast path in the bool-overload when `set_flags` is false (masked
      sum only); pair-overload untouched. GRL.

## 8. Wrap-up

- [ ] 8.1 Fill the final numbers table (per-item and cumulative, 3-run
      ranges) in `openspec/changes/cache-hotpath-tier1/numbers.md`.
- [ ] 8.2 Run the full test suite once more (both presets) + `--verify`;
      confirm every commit message references its task ID.
- [ ] 8.3 Leave the branch unmerged; summary of measured wins vs paper
      estimates recorded in numbers.md (do NOT merge to main — explicit
      instruction).
