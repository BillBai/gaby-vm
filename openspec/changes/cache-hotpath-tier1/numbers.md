# cache-hotpath-tier1 — measured numbers

Host: Apple M1 Pro, macOS 26.5.1, AC power, machine otherwise idle during
timing. Build: `dev-release` preset (`-O3 -DNDEBUG`), `bench_business
--mode cache --seconds 1.0`, 3 runs per row, values are ns/insn.
These numbers are NOT comparable to the M4 Pro doc baselines
(`docs/refs/gaby-vm-business-bench-*.md`).

## Baseline (commit 03a4a03, no code changes)

| run | parse | hash | struct | fsm | applogic |
|-----|------:|-----:|-------:|----:|---------:|
| 1 | 9.371 | 10.285 | 10.808 | 9.344 | 14.301 |
| 2 | 9.359 | 10.260 | 10.788 | 9.356 | 14.326 |
| 3 | 9.364 | 10.491 | 10.780 | 9.343 | 14.370 |
| med | 9.364 | 10.285 | 10.788 | 9.344 | 14.326 |

Guard rails at baseline: `bench_business --verify` OK (all kernels,
cache == decoder, x0 matches committed oracle); `ctest --test-dir
build/debug -R vixl_port` 3/3 passed (2026-07-03).

## Per-item results

(filled in as tasks land; each row = median of 3 runs after that item's
commit, cumulative on the branch)

| after | parse | hash | struct | fsm | applogic | notes |
|-------|------:|-----:|-------:|----:|---------:|-------|
| T1 (LogicVRegister) | 9.395 | 10.282 | 10.702 | 9.360 | 11.335 | applogic -20.9% vs baseline; scalar within noise (±0.8%). |
| T2a (trace tail + guard bounds) | 8.996 | 10.341 | 9.966 | 9.254 | 10.783 | vs T1: parse -4.2%, struct -6.9%, applogic -4.9%, fsm -1.1%; hash +0.6% (noise). |
| T2b (MaybeClear LCG) | 8.671 | 10.354 | 9.476 | 9.137 | 10.603 | vs T2a: parse -3.6%, struct -4.9%, applogic -1.7%, fsm -1.3%; hash +0.1% (noise). |
| T3 (MOVPRFX flag) | 8.659 | 10.426 | 9.561 | 9.096 | 10.597 | performance-neutral (see T3 detail); landed for spec/structural reasons. |
| T4 (hub epilogue) | | | | | | |
| T5 (interception flag) | | | | | | |
| T6 (AddWithCarry) | | | | | | go/no-go per task 1.2 |

### T1 detail (task 2.5)

Three `--mode cache --seconds 1.0` runs after the T1 commit (median in the
row above), ns/insn:

| run | parse | hash | struct | fsm | applogic |
|-----|------:|-----:|-------:|----:|---------:|
| 1 | 9.395 | 10.365 | 10.694 | 9.336 | 11.334 |
| 2 | 9.400 | 10.190 | 10.710 | 9.360 | 11.335 |
| 3 | 9.362 | 10.282 | 10.702 | 9.355 | 11.338 |
| med | 9.395 | 10.282 | 10.702 | 9.360 | 11.335 |

applogic runs: 11.334 / 11.335 / 11.338 (median 11.335), down from the 14.326
baseline — a 20.9% drop, at the strong end of the ~14.3 → ~10-11 paper estimate.
Scalar shapes are unchanged within run-to-run noise (parse +0.3%, hash -0.0%,
struct -0.8%, fsm +0.2%). `bench_business --verify` OK (cache == decoder for all
kernels); `ctest -R vixl_port` 3/3; full debug ctest 24/24.

Implementation note: task 2.3 (`SimRegisterBase::Write` clear) was bounded to a
**compile-time constant** `min(kMaxSizeInBytes, kZRegMinSizeInBytes)`, not the
runtime `size_in_bytes_`. A first cut using the runtime length (via `ClearTail`)
regressed the scalar kernels ~5% consistently across 3 runs because it turned
the scalar W-register write's single-store clear into a general `memset` call;
the constant bound keeps the scalar path single-store while still shrinking the
V/Z clear from 256B to 16B, so it recovers the scalar cost and also improves
applogic over the runtime-length version (11.3 vs 12.3).

### T2a detail (task 3.3) — trace tail gate + guard-region bounds

Two changes in one commit (tasks 3.1 + 3.2), cumulative on top of T1:

- 3.1 gates the dead trace-preparation tail in `LoadStoreHelper` /
  `LoadStorePairHelper` behind a single `GetTraceParameters() != 0` test (the
  same mask the `Log*` helpers already test bit-by-bit), so with tracing off
  the second data-size derivation, the `GetPrintRegisterFormatForSize[TryFP]`
  runtime switch, and the cross-TU `IsLoad()/IsStore()` classification are
  skipped. `local_monitor_.MaybeClear()` stays unconditional.
- 3.2 caches the inclusive allocation bounds in `SimStack::Allocated` at
  `Allocate()` time and rewrites `IsAccessInGuardRegion` to a two-compare
  fast-out (access does not overlap the allocation → not a guard access,
  which is the common case: every heap/code access) followed by a two-compare
  in-guard test, bit-identical to the original four-compare form.

Three `--mode cache --seconds 1.0` runs after the commit (median in the row
above), ns/insn:

| run | parse | hash | struct | fsm | applogic |
|-----|------:|-----:|-------:|----:|---------:|
| 1 | 9.003 | 10.371 | 9.966 | 9.254 | 10.783 |
| 2 | 8.996 | 10.341 | 9.940 | 9.213 | 10.888 |
| 3 | 8.984 | 10.273 | 9.968 | 9.257 | 10.773 |
| med | 8.996 | 10.341 | 9.966 | 9.254 | 10.783 |

vs the T1 row: the load/store-heavy shapes move as predicted — parse
9.395 → 8.996 (-4.2%), struct 10.702 → 9.966 (-6.9%). applogic 11.335 → 10.783
(-4.9%) and fsm 9.360 → 9.254 (-1.1%) also improve (both do memory work).
hash 10.282 → 10.341 (+0.6%) is compute-bound and stays within run-to-run
noise (per-run deltas +0.9% / +0.6% / -0.1%; no shape regresses >2% in any of
the three runs). `bench_business --verify` OK (cache == decoder for all
kernels); `ctest -R vixl_port` 3/3; full debug ctest 24/24.

Trace-ON identity is by construction: the 3.1 gate tests the exact
`GetTraceParameters()` value each `Log*` call tests, so with any trace bit set
the whole tail runs unchanged and with none set every `Log*` is a no-op. No
in-repo test compares printed simulator trace output — `VIXL_PORT_TRACE` only
drives harness `[run]` logging, not `SetTraceParameters` — so the correctness
guard rails cover state equivalence (24/24 ctest, `vixl_port` 3/3, `--verify`
bit-identical) rather than byte-for-byte trace text.

### T2b detail (task 3.4) — MaybeClear LCG early-return (own commit, design D5)

`SimExclusiveLocalMonitor::MaybeClear` ran a 64-bit multiply + modulo LCG on
every memory access even when no exclusive reservation is held. When the
monitor is unarmed (`size_ == 0`) `Clear()` is a no-op, so the upstream body's
only effect is stepping the seed; the business kernels never arm the monitor
(no ldxr/stxr), so the early return fires on every memory access. Own commit,
marker comment naming the design-D5 hazard: this shifts the LCG seed *sequence*
vs upstream VIXL, harmless because all three in-repo simulators share the class
(oracles stay aligned) but a future island re-sync importing an upstream test
that asserts a specific STXR status without a retry loop could diverge — revert
this commit in isolation if that ever happens.

Three `--mode cache --seconds 1.0` runs after the commit (median in the row
above), ns/insn:

| run | parse | hash | struct | fsm | applogic |
|-----|------:|-----:|-------:|----:|---------:|
| 1 | 8.671 | 10.259 | 9.484 | 9.137 | 10.603 |
| 2 | 8.670 | 10.514 | 9.476 | 9.122 | 10.633 |
| 3 | 8.697 | 10.354 | 9.475 | 9.170 | 10.600 |
| med | 8.671 | 10.354 | 9.476 | 9.137 | 10.603 |

vs the T2a row: parse 8.996 → 8.671 (-3.6%), struct 9.966 → 9.476 (-4.9%),
applogic 10.783 → 10.603 (-1.7%), fsm 9.254 → 9.137 (-1.3%); hash
10.341 → 10.354 (+0.1%, per-run -0.8% / +1.7% / +0.1%, within noise, no >2%
regression in any run). `bench_business --verify` OK (cache == decoder);
`ctest -R vixl_port` 3/3; full debug ctest 24/24.

Cumulative T2 (a+b) vs the pre-T2 T1 row: parse 9.395 → 8.671 (-7.7%),
struct 10.702 → 9.476 (-11.5%), applogic 11.335 → 10.603 (-6.5%),
fsm 9.360 → 9.137 (-2.4%), hash 10.282 → 10.354 (+0.7%, noise).

### T3 detail (task 4.3) — neutral by measurement, landed by adjudication

Three post-change runs (medians in the row above): parse 8.659 / hash
10.426 / struct 9.561 / fsm 9.096 / applogic 10.597. Because session drift
was on the order of the expected effect, a controlled same-session A/B
(stash → rebuild HEAD → 3 runs → pop → rebuild T3 → 3 runs) measured:
parse +0.14%, hash +1.7% (noisy compute-bound shape, no consistent >2%
regression), struct +0.57%, fsm −0.51%, applogic −0.05% — i.e. **neutral**.

Why the 2026-06-11 ablation's ~6% did not materialize here: the ablation
deleted the *entire* MOVPRFX machinery (branch + cross-call bool +
derivation); the behavior-preserving formulation must keep the `form_hash_`
store (leaves depend on it) and the post-leaf check with a bool live across
the leaf call (a pre-leaf check would inspect the wrong `pc_` for
branch-shaped illegal consumers). What remains removable — two constant
compares — is hidden by the out-of-order core behind the indirect leaf call.

Landed anyway per the design's acceptance criterion ("each item ≥ neutral"):
it realizes the delta-spec scenarios (predecode-derived classification,
check still enforced, cursor-covered state), and it removes the hub's
per-step `form_hash_` *read* dependency — a structural precondition for the
Tier-2 flatten work. Guard rails all green: full debug ctest 24/24,
vixl_port 3/3, movprfx negative test aborts at the same `CanTakeSVEMovprfx`
check, `bench_business --verify` OK. Adjudication: Fable orchestrator,
2026-07-03 (the implementing agent stopped at the task 4.3 bench gate as
instructed; the design's ≥-neutral criterion takes precedence over the
brief's 1.5% execution threshold).

## T6 disassembly gate (task 1.2)

**Verdict: NO-OP — T6 skipped.** In the dev-release binary, clang -O3
already sinks the entire NZCV derivation behind the `set_flags` test in
both copies of the code: the standalone
`AddWithCarry(unsigned, bool, uint64_t, uint64_t, int)` computes the masked
sum then `cbz w2, <ret>` skips ~35 flag instructions when `set_flags` is
false; the copy inlined into `AddSubHelper` gates the same ~30-instruction
flag block on `tbnz w21, #29` (the instruction's S bit). The
`set_flags == false` path already executes only add-and-mask. A source-level
early return would remove zero instructions. (Analysis: Opus subagent,
2026-07-03; predicted by the exploration review's D6/B-P2 caveat.)
