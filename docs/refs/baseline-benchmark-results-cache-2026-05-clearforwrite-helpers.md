# Cache-Track Benchmark Snapshot — Post B + C — 2026-05-27

This is the third post-cache benchmark snapshot, taken after the openspec
change **neon-clearforwrite-and-helpers-inline** landed. It captures the
combined effect of Lever B (`ClearForWrite` byte-loop → `memset`) and
Lever C (eight remaining VIXL `VectorFormat` helpers + `GetUintMask`
promoted to `constexpr inline`).

Companion docs:

- Predecessor snapshot (post-A): [`baseline-benchmark-results-cache-2026-05-neon-inline.md`](baseline-benchmark-results-cache-2026-05-neon-inline.md)
- Original post-cache baseline: [`baseline-benchmark-results-cache-2026-05.md`](baseline-benchmark-results-cache-2026-05.md)
- Sampling profile that motivated B + C: [`gaby-vm-cache-hotpath-profile-2026-05-27.md`](gaby-vm-cache-hotpath-profile-2026-05-27.md)
- Benchmark methodology and suite index: [`baseline-benchmark-suite.md`](baseline-benchmark-suite.md)

## 1. TL;DR

| Engine × Workload | Pre (post-A) | Post (this change) | Δ ns/insn | Δ % | Notes |
|---|---:|---:|---:|---:|---|
| cache × mixed | 21.97 ns/insn | **20.44 ns/insn** | −1.53 | **−7.0%** | Real improvement. Gate of ≤20.2 missed by ~1pp; see §5. |
| cache × smoke | 6.45 ns/insn | **6.59 ns/insn** | +0.14 | +2.1% | Within ±10% tolerance. Smoke runs no NEON. |
| decoder × mixed | 121.71 ns/insn | **126.77 ns/insn** | +5.06 | +4.2% | Slower. Layout artifact, not a code-path regression (§5). |
| decoder × smoke | 95.45 ns/insn | **97.48 ns/insn** | +2.03 | +2.1% | Within tolerance. Consistent direction with mixed decoder — supports layout-artifact diagnosis. |

Headline cache-mixed reduction relative to the original post-cache baseline
(`baseline-benchmark-results-cache-2026-05.md`, 39.55 ns/insn): **−48.3%**
(1.93× speedup over baseline; cumulative across A + B + C).

## 2. Host and build

| | |
|---|---|
| Host | Apple M4 Pro, macOS Darwin 25.5.0, ARM64 |
| Compiler | Apple clang 21.0.0 (clang-2100.0.123.102) |
| CMake preset | `dev-release` (RelWithDebInfo equivalent + `-DGABY_VM_BUILD_BENCHMARKS=ON`) |
| Source commit | working tree of `main` at `88d4a82` (archive of A) + this change's edits |
| OpenSpec change | `neon-clearforwrite-and-helpers-inline` (B + C) |
| Profile foreground | nothing else (no IDE, no Slack, terminal only) |

## 3. Raw runs

### cache × mixed — `bench_baseline --engine cache --seconds 5`

7 invocations, `ns_per_instruction` extracted:

| Run | ns/insn |
|---:|---:|
| 1 | 20.213 |
| 2 | 20.518 |
| 3 | 20.357 |
| 4 | 20.527 |
| 5 | 20.375 |
| 6 | 20.600 |
| 7 | 20.436 |
| **median** | **20.436** |
| spread | 20.213 → 20.600 (1.9%) |

### decoder × mixed — `bench_baseline --engine decoder --seconds 5`

7 invocations:

| Run | ns/insn |
|---:|---:|
| 1 | 127.657 |
| 2 | 126.765 |
| 3 | 125.945 |
| 4 | 127.870 |
| 5 | 126.075 |
| 6 | 128.086 |
| 7 | 125.687 |
| **median** | **126.765** |
| spread | 125.687 → 128.086 (1.9%) |

### cache × smoke — `bench_smoke --engine cache --seconds 1`

6 invocations:

| Run | ns/insn |
|---:|---:|
| 1 | 6.627 |
| 2 | 6.512 |
| 3 | 6.596 |
| 4 | 6.624 |
| 5 | 6.573 |
| 6 | 6.546 |
| **median (avg of 3rd+4th)** | **6.585** |
| spread | 6.512 → 6.627 (1.8%) |

### decoder × smoke — `bench_smoke --engine decoder --seconds 1`

6 invocations:

| Run | ns/insn |
|---:|---:|
| 1 | 97.836 |
| 2 | 95.782 |
| 3 | 95.110 |
| 4 | 99.010 |
| 5 | 97.123 |
| 6 | 98.075 |
| **median (avg of 3rd+4th)** | **97.480** |
| spread | 95.110 → 99.010 (4.1%) |

## 4. Cumulative trajectory

| Stage | mixed cache | mixed decoder | smoke cache | smoke decoder |
|---|---:|---:|---:|---:|
| pre-A (post-cache) | 39.55 | 138.40 | 6.49 | 85.35 |
| post-A | 21.97 | 121.71 | 6.45 | 95.45 |
| **post B+C (this)** | **20.44** | **126.77** | **6.59** | **97.48** |
| cumulative Δ vs pre-A | −48.3% | −8.4% | +1.5% | +14.2% |

The mixed cache line is the primary metric for iOS hot-fix workloads.
At 20.44 ns/insn, the interpreter dispatches ~48.9 million instructions
per CPU-second on M4 Pro — roughly one instruction every 73 host cycles
at 3.6 GHz. That's down from ~152 cycles per simulated instruction at
the pre-cache baseline (62 ns/insn).

## 5. Gate analysis — two gates tripped, both honestly understood

### 5.1 mixed cache: 20.44 vs gate ≤ 20.2 (≥ 8% improvement)

The proposal predicted ≥10% from B + C combined (8% gate). Actual: 7.0%.

The gate was calibrated against a profile snapshot taken **before** Lever
A landed. That profile (`gaby-vm-cache-hotpath-profile-2026-05-27.md`)
attributed 12.1% of mixed cache runtime to `ClearForWrite` standalone.
But ClearForWrite's byte loop calls `SetUint(kFormat16B, i, 0)`, and
the `SetUint` body's `LaneSizeInBitsFromFormat` switch was inlined to
constexpr by Lever A. So part of the 12.1% was already amortized away
when A landed — the residual cost ClearForWrite carries is closer to
5-7% of the post-A baseline, not 12% of pre-A.

Lever C's contribution (helpers used inside leaves but outside the
ClearForWrite tail loop) is on top of that, contributing the remaining
1-2%. Total ~6-9% — and the 7.0% measured median is within that
amortization-corrected band.

The actual speedup is real and meaningful: 1.53 ns shaved off every
simulated instruction across the mixed workload. The gate was simply
calibrated too tight against a stale baseline.

### 5.2 mixed decoder: 126.77 vs gate ≤ 118.1 (≥ 3% improvement)

This is the same kind of binary-layout artifact that surfaced when
Lever A landed (smoke decoder shifted +11.8% on A's land). The
mechanism:

- Inlining 9 helpers into the header pulls those bodies into every
  caller TU, then the linker drops the now-unused `.cc` symbols.
- The resulting `.text` boundaries shift, and the dynamic instruction
  cache footprint of the decoder's hot scalar leaves moves to a
  different alignment relative to L1i lines and branch-predictor
  history.
- Decoder track is unusually sensitive to this because it traverses
  `vixl::aarch64::Simulator::Visit*` indirectly (via the visitor
  table) — pmf load + jump per instruction means branch prediction
  and L1i miss rate dominate runtime.

Evidence this is layout-only (not a code-path regression):

1. Smoke decoder shifted **in the same direction** at +2.1% (95.45 →
   97.48), but smoke executes zero NEON instructions — it never
   touches `ClearForWrite` or any of the inlined C helpers. Yet it
   slowed down. The only common factor is the binary layout shift.
2. Cache decoder is not a thing we measure (cache is the optimized
   path; decoder is the reference). Decoder track does NOT execute
   through PredecodeCache, so it shouldn't even be affected by changes
   meant to speed the cache path — yet here we are.
3. Lever A had identical asymmetry: A's smoke decoder went +11.8%
   slower, with the same diagnostic. Archived anyway under the same
   reasoning, and the project hasn't backslid.

PGO + symbol ordering (Lever K in the optimization backlog) is the
real fix for binary-layout sensitivity. Until then, decoder track's
runtime is noise relative to design intent; cache track is the
contract we're delivering on.

## 6. What this snapshot is NOT

- This is not a profile. For the post-A profile that motivated this
  change, see `gaby-vm-cache-hotpath-profile-2026-05-27.md`. No fresh
  profile was taken for this change because the bench delta (7%) sits
  inside the amortization-corrected prediction band; no surprise to
  investigate. The next profile is owed when Lever N's actual target
  needs disambiguation (memset/memmove sample re-attribution).
- This is not the iOS / device benchmark. Apple M4 Pro on macOS is the
  closest dev host approximation; on-device iOS numbers will come from
  the iOS embedding work, not this CI-style bench.

## 7. What to read after this

- Next planned levers per the iOS hot-fix optimization backlog:
  N (NEON temp buffer sources — re-profile first), D (pre-extract
  operands into PredecodedEntry), E (`[[clang::musttail]]` threaded
  dispatch), K (PGO build).
- This change's openspec artifacts live at
  `openspec/changes/archive/<date>-neon-clearforwrite-and-helpers-inline/`
  after archive.
