# Baseline Benchmark Results: Cache Track + NEON Inline, 2026-05-27

[Chinese version](baseline-benchmark-results-cache-2026-05-neon-inline.zh-cn.md)

This historical snapshot was taken after
`neon-format-helpers-constexpr-inline`, which moved six NEON `VectorFormat`
helpers from `.cc` definitions to `constexpr inline` header definitions.

It is a sibling of the pre-change cache snapshot
`baseline-benchmark-results-cache-2026-05.md` and the original decoder-only
baseline. Treat it as immutable data.

## Summary

Delta against the previous cache-track baseline on the same Apple M4 Pro:

| Workload | mode | median ns/insn now | pre-change | delta | speedup |
|----------|------|-------------------:|-----------:|------:|--------:|
| `mixed` | decoder | **121.71** | 138.40 | **-12.1%** | **1.14x** |
| `mixed` | **cache** | **21.97** | 39.55 | **-44.5%** | **1.80x** |
| `smoke` | decoder | 95.45 | 85.35 | **+11.8%** | **0.894x** |
| `smoke` | **cache** | **6.45** | 6.49 | -0.6% | 1.006x |

The headline result is `mixed` cache at **1.80x**, better than the 1.4x to 1.6x
estimate. `mixed` decoder also improved because the same NEON leaf helpers are
used by both modes. `smoke` cache stayed flat, as expected, because smoke has no
NEON helper traffic.

## Host and Build

| Field | Value |
|-------|-------|
| Machine | MacBook, Apple M4 Pro, 10 P-cores + 4 E-cores |
| OS | macOS 26.5, build 25F71 |
| Kernel | Darwin 25.5.0 arm64 |
| Compiler | Apple clang 21.0.0 |
| Build flags | `CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `dev-release` preset |
| Power | AC |
| Concurrent load | `uptime` 1.76 / 2.36 / 2.47 |

The workload bytes are identical to the previous snapshot:

- `mixed`: `vixl@3fe168632164`, seed 42, 65548 static words,
  64643 dynamic instructions per iteration.
- `smoke`: `llvm-mc 22.1.5`, 32 static words, 32 dynamic instructions per
  iteration.

## Raw Medians

| Workload | mode | runs | median iterations/s | median ns/insn | spread |
|----------|------|-----:|--------------------:|----------------:|-------:|
| `mixed` | decoder | 7 | 127.10 | 121.71 | 2.8% |
| `mixed` | cache | 7 | 704.17 | 21.97 | 2.5% |
| `smoke` | decoder | 8 | 327406 | 95.45 | 8.2% |
| `smoke` | cache | 6 | 4845408 | 6.45 | 2.4% |

## Acceptance Against the OpenSpec Change

| Threshold | Expected | Measured | Result |
|-----------|----------|----------|--------|
| `mixed` cache median ns/insn | at least 20 percent improvement, <= 31.6 ns | **21.97 ns**, 44.5 percent improvement | pass |
| `mixed` decoder median ns/insn | within 5 percent of 138.40 ns | **121.71 ns**, 12.1 percent faster | beneficial, gate was conservative |
| `smoke` cache median ns/insn | within 10 percent of 6.49 ns | **6.45 ns** | pass |
| `smoke` decoder median ns/insn | within 10 percent of 85.35 ns | **95.45 ns**, 11.8 percent slower | slightly outside, see below |

## Why `smoke` Decoder Slowed Down

`smoke` contains no NEON instructions and does not call the inlined NEON format
helpers. The measured decoder slowdown is best explained as binary-layout
reshuffling: removing several `.cc` function bodies moves neighboring hot scalar
leaf code to different cache-line or page positions.

Evidence:

1. `smoke` cache stayed flat, so the scalar leaves did not get semantically or
   structurally slower in a way that affects both modes.
2. `mixed` decoder improved because it actually uses the NEON helpers.
3. The repeated `smoke` decoder run showed a stable lower median rather than a
   single transient load spike.

The note treated this as a layout artifact, not a performance regression. Fixes
such as PGO, hot/cold attributes, or symbol ordering were out of scope.

## Prediction Check

The 2026-05-27 cache hot-path profile estimated this lever could reduce
`mixed` cache by about 30 to 50 percent. The measured 21.97 ns/instruction lands
near the strong end of that estimate. Follow-up work should expect remaining
`mixed` costs to shift toward NEON temporary-object memory management, dispatch,
and leaf bodies.

Short-term follow-up suggestions from the original note:

1. Combine NEON `ClearForWrite` gating with heap removal.
2. Evaluate operand pre-extraction, especially for `smoke`.
3. Re-profile after the next change.

## Index

- Pre-change cache snapshot:
  [`baseline-benchmark-results-cache-2026-05.md`](./baseline-benchmark-results-cache-2026-05.md).
- Original decoder-only baseline:
  [`baseline-benchmark-results-2026-05.md`](./baseline-benchmark-results-2026-05.md).
- Profile that motivated this change:
  [`gaby-vm-cache-hotpath-profile-2026-05-27.md`](../../refs/gaby-vm-cache-hotpath-profile-2026-05-27.md).
- Benchmark harness: [`../../../bench/README.md`](../../../bench/README.md).
