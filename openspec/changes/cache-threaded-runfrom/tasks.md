# Tasks: cache-threaded-runfrom

GRL = build dev-debug + dev-release → `ctest --test-dir build/debug -R
vixl_port` 3/3 → full debug ctest green → `bench_business --verify` OK →
3× `bench_business --mode cache --seconds 1.0` per shape → numbers.md.
Before-reference (C1 row): parse 8.116 / hash 6.654 / struct 8.648 /
fsm 8.057 / applogic 9.918.

## 1. Mechanics

- [ ] 1.1 `gaby_step_mode_` member + ExecutionScope save/set/restore
      (design D2); `GABY_THREADED` / `GABY_MUSTTAIL` build plumbing with
      the loop fallback (design D1).
- [ ] 1.2 Boundary sentinel: populate `size/4 + 1` entries, boundary
      handler (re-resolve / terminal / abort), `CodeRange` comment
      (design D4).
- [ ] 1.3 Continuation protocol in the thunk epilogue + terminal handler +
      null-PC routing (design D3); `GABY_DISPATCH` macro with step-mode
      gate (design D1).
- [ ] 1.4 `RunFrom` threaded harness + loop fallback; `StepOnce` via the
      gate, externally unchanged (design D5).

## 2. Tests

- [ ] 2.1 Whole-run equivalence test: threaded vs stepped vs decoder on a
      branchy multi-block range (spec scenario 1).
- [ ] 2.2 Hook-in-chain re-entry test (nested RunFrom from a hook
      mid-chain, both nesting directions) + hook-terminated-run test
      (spec scenarios 2-3).
- [ ] 2.3 Boundary tests: adjacent-range fallthrough; run-off-end abort
      (spec scenarios 4-5). Deep-chain smoke: ~1M straight-line
      instructions execute without stack growth (musttail structural
      check).
- [ ] 2.4 GRL + numbers.md row/detail (accept ≥ neutral; expect
      applogic ~8.4-9.2; STOP on >2% consistent regression, A/B before
      concluding). Commit 1.

## 3. CI + docs

- [ ] 3.1 CI jobs per design D7: threaded `-O0` build+ctest; and a
      `GABY_THREADED=0` fallback build+ctest (follow `ci/` + workflow
      conventions in docs/refs/ci.md).
- [ ] 3.2 Design-note supersession (direct-threading non-goal in
      `docs/refs/gaby-vm-predecode-cache-design.md`); tick tasks; commit 2.
