# Baseline Benchmark Results: Cache Track, 2026-05

[Chinese version](baseline-benchmark-results-cache-2026-05.zh-cn.md)

This historical throughput snapshot compares the gaby-vm cache mode against
the imported VIXL decoder mode on identical workloads. It is the first benchmark
record after cache mode existed, following the earlier decoder-only
`baseline-benchmark-results-2026-05.md`.

Treat this as immutable historical data. New runs should be recorded in new
sibling files.

## Summary

Same Apple M4 Pro P-core, single thread, AC power, no pinning:

| Workload | mode | median throughput (insn/s) | median ns/insn | median it/s |
|----------|------|---------------------------:|---------------:|------------:|
| `mixed` | decoder | **7.23 M** | 138.40 | 111.77 |
| `mixed` | **cache** | **25.29 M** | **39.55** | **391.19** |
| `smoke` | decoder | **11.72 M** | 85.35 | 366151 |
| `smoke` | **cache** | **154.14 M** | **6.49** | **4816783** |

Cache speedup versus decoder:

- `mixed`: about **3.50x**.
- `smoke`: about **13.16x**.

`mixed` sits at the low end of the earlier expected range because NEON and
memory leaves dominate its cost. `smoke` benefits more because it is a tiny
branch-free ALU sequence where cache mode removes most decode/visitor overhead.

Order-of-magnitude framing:

- M4 Pro native throughput is roughly `1e10` instructions/s.
- Cache `mixed` is about 400x slower than native.
- Cache `smoke` is about 65x slower than native.
- Assuming about 4.5 GHz boost, cache `mixed` costs roughly 180 host cycles per
  guest instruction, and cache `smoke` roughly 29 cycles.

## Host

| Field | Value |
|-------|-------|
| Machine | MacBook, Apple M4 Pro |
| CPU | 10 P-cores + 4 E-cores |
| OS | macOS 26.5, build 25F71 |
| Kernel | Darwin 25.5.0 arm64 |
| Compiler | Apple clang 21.0.0 |
| Build flags | `CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `dev-release` preset |
| Power | AC, fan-cooled |
| Concurrent load | `uptime` 1.98 / 4.27 / 4.26 |

Compared with the previous 2026-05 baseline, the OS/kernel point release moved
and the 5/15 minute load averages were higher. Median interpretation remains
order-of-magnitude stable.

## Build and Workload Provenance

- Repository commit: `16f27a9355efa6035a1b3fc3bd94c71898402a87`.
- Binary-equivalent performance commit: `ca7b580`, the cache hot-path speedup
  that removed `std::function` indirection and gated BType checks.
- Configure: `cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON`.
- Binaries: `build/release/bench/bench_baseline`,
  `build/release/bench/bench_smoke`.

Workloads:

- `mixed`: `workload_generator_tag: vixl@3fe168632164; seed=42;
  buffer_bytes=262192`, `static_words_in_buffer=65548`,
  `dynamic_instructions_per_iteration=64643`.
- `smoke`: `workload_generator_tag: llvm-mc 22.1.5;
  source_sha256=4769ba17a5fe`, `static_words_in_buffer=32`,
  `dynamic_instructions_per_iteration=32`.

## Raw Medians

The original note recorded all runs. Median values are:

| Workload | mode | runs | median iterations/s | median throughput (insn/s) | median ns/insn | spread on it/s |
|----------|------|-----:|--------------------:|---------------------------:|---------------:|---------------:|
| `mixed` | decoder | 7 | 111.77 | 7225271 | 138.40 | 3.9% |
| `mixed` | cache | 7 | 391.19 | 25287641 | 39.55 | 1.8% |
| `smoke` | decoder | 6 | 366151 | 11716816 | 85.35 | 3.7% |
| `smoke` | cache | 6 | 4816783 | 154137062 | 6.49 | 4.4% |

All spreads were under 5 percent, so the medians were stable enough for this
historical comparison.

## Decoder Baseline Check

Decoder mode should not change because cache-only changes do not touch that
path. Compared with the earlier decoder-only baseline:

| Workload | old decoder median (insn/s) | new decoder median (insn/s) | delta |
|----------|----------------------------:|----------------------------:|------:|
| `mixed` | 7460515 | 7225271 | -3.2% |
| `smoke` | 12474489 | 11716816 | -6.1% |

These changes fit host noise and OS/load differences. The cache hot-path
speedup did not show a meaningful decoder-side regression.

## Correction to the Earlier 57 ns/instruction Profile Number

An earlier cache hot-path profile reported `mixed` cache at about
57 ns/instruction. This benchmark measured 39.55 ns/instruction without
sampling. A later 2026-05-27 profile showed that profile-build sampling overhead
was only about 3 percent for `mixed` and under 1 percent for `smoke`, not 30
percent. The old 57 ns/instruction absolute number should not be used as the
steady-state cache cost. The profile's percentage breakdown remained useful.

## What This Measures

Included:

- decoder mode: imported VIXL decode tree, visitor chain, and leaves;
- cache mode: steady-state cache-hit execution, including BType gate,
  `IncrementPc`, and `UpdateBType`;
- per-iteration LR reset before `RunFrom`.

Not included:

- predecode construction cost;
- large-cache capacity pressure beyond this workload;
- multi-thread or multi-instance contention;
- iOS-device measurements.

## Reproduction

```sh
cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON
cmake --build --preset dev-release --target bench_baseline bench_smoke

./build/release/bench/bench_baseline --mode decoder --seconds 5
./build/release/bench/bench_baseline --mode cache --seconds 5
./build/release/bench/bench_smoke --mode decoder --seconds 1
./build/release/bench/bench_smoke --mode cache --seconds 1
```

Primary metric: `iterations_per_second`. `throughput_insn_per_sec` and
`ns_per_instruction` derive from it.

## Comparison Targets for Later Work

When using this as a baseline:

- decoder should stay within about 5 percent unless the decoder path changed;
- NEON helper work should primarily move cache `mixed`;
- cache `smoke` already has little slack without larger dispatch or register
  caching work.

## Index

- Previous decoder-only snapshot:
  [`baseline-benchmark-results-2026-05.md`](./baseline-benchmark-results-2026-05.md).
- Related profile:
  [`gaby-vm-cache-hotpath-profile-2026-05-27.md`](../../refs/gaby-vm-cache-hotpath-profile-2026-05-27.md).
- Cache design:
  [`../../refs/gaby-vm-predecode-cache-design.md`](../../refs/gaby-vm-predecode-cache-design.md).
- Benchmark harness:
  [`../../../bench/README.md`](../../../bench/README.md).
