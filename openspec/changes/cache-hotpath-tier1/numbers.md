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
| T2a (trace tail + guard bounds) | | | | | | |
| T2b (MaybeClear LCG) | | | | | | |
| T3 (MOVPRFX flag) | | | | | | |
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
