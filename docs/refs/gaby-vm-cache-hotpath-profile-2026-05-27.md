# Cache Hot-Path Profile, 2026-05-27

[Chinese version](gaby-vm-cache-hotpath-profile-2026-05-27.zh-cn.md)

This is the second targeted sampling profile of the cache path. It uses the
same method as the earlier archived profile: macOS `sample`, 1 ms interval, and
`build/profile/` flags `-O3 -g -fno-omit-frame-pointer -DNDEBUG`. Code state was
commit `16f27a9`, equivalent to `ca7b580` for binaries with only docs changed.

This profile added three corrections:

1. It adds the missing `smoke` profile. `smoke` and `mixed` have almost
   orthogonal hot spots.
2. It corrects sampling overhead. Profile build versus dev-release was
   40.69 vs 39.55 ns/instruction for `mixed`, about +3 percent, and
   6.53 vs 6.49 ns/instruction for `smoke`, under 1 percent. The previous
   estimate of roughly 30 percent sampling interference was wrong. The earlier
   `mixed` cache number near 57 ns/instruction should not be used as the cache
   path cost reference.
3. It adds a visible benchmark-harness overhead bucket for `smoke`, about
   5 percent.

## Summary

- `mixed` cache mode, 39.55 ns/instruction: about 68 percent of time is NEON
  abstraction cost: format helpers, `ClearForWrite`, `memset`/`memmove`, and
  `malloc`/`free`. Dispatch itself, `ExecuteInstructionCached` plus
  `gaby_vm::Simulator::RunFrom`, is only 6.4 percent.
- `smoke` cache mode, 6.49 ns/instruction: the distribution is different.
  Dispatch is 37 percent, leaves are 42 percent, and the rest is `Visit*` entry
  cost plus benchmark timer overhead. This is the non-NEON limit case for the
  cache path.
- Optimization paths do not fully overlap. NEON abstraction work helps `mixed`
  and does almost nothing for `smoke`. Dispatch-loop work helps `smoke` first
  and has only a small ceiling on `mixed`.

| Category | mixed | smoke | Notes |
|----------|------:|------:|-------|
| NEON format helpers, `ClearForWrite`, `memset`/`memmove` | **~65.7%** | 0% | First priority for `mixed`. |
| Temporary `LogicVRegister` / `SimVRegister` heap allocation | ~2.5% | 0% | `mixed` only. |
| Dispatch, `ExecuteInstructionCached` plus `RunFrom` | ~6.4% | **~37.4%** | First priority for `smoke`. |
| `Visit*` entry cost | ~5% | ~10.9% | Present in both. |
| Leaf helpers | ~20% | ~42.3% | Present in both, heavier in `smoke`. |
| Benchmark timer and LR reset | <0.1% | ~5.4% | Measurement artifact for 32-insn `smoke`. |

## 1. Measurement Setup

```bash
cmake -S . -B build/profile -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGABY_VM_BUILD_BENCHMARKS=ON \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -fno-omit-frame-pointer -DNDEBUG"
cmake --build build/profile --target bench_baseline bench_smoke

./build/profile/bench/bench_baseline --engine cache --seconds 15 &
sample $(pgrep -f bench_baseline | head -1) 10 -mayDie \
    -file /tmp/mixed_cache_profile_2026_05_27.txt

./build/profile/bench/bench_smoke --engine cache --seconds 10 &
sample $(pgrep -f bench_smoke | head -1) 8 -mayDie \
    -file /tmp/smoke_cache_profile_2026_05_27.txt
```

- Machine: Apple M4 Pro, macOS 26.5, no pinning.
- Sample counts: 8566 for `mixed`, 6880 for `smoke`, main thread totals from
  the `sample` call graph header.
- Throughput:
  - `mixed` profile build: 40.69 ns/instruction, dev-release: 39.55.
  - `smoke` profile build: 6.53 ns/instruction, dev-release: 6.49.
- Raw local dumps:
  `/tmp/mixed_cache_profile_2026_05_27.txt` and
  `/tmp/smoke_cache_profile_2026_05_27.txt`.

## 2. Mixed Top-of-Stack Counts

Only entries with at least 10 samples are listed. Bold rows are imported VIXL
helpers worth considering.

| Samples | Percent | Function |
|--------:|--------:|----------|
| **2253** | **26.3%** | `vixl::aarch64::LaneSizeInBitsFromFormat(VectorFormat)` |
| **1517** | **17.7%** | `vixl::aarch64::IsSVEFormat(VectorFormat)` |
| **1036** | **12.1%** | `vixl::aarch64::LogicVRegister::ClearForWrite(VectorFormat)` |
| 496 | 5.8% | `vixl::aarch64::Simulator::ExecuteInstructionCached()` |
| 343 | 4.0% | `_platform_memset` |
| 300 | 3.5% | `_platform_memmove` |
| 177 | 2.1% | `Simulator::LoadStoreHelper` |
| 166 | 1.9% | `Simulator::LoadStorePairHelper` |
| **118** | 1.4% | `vixl::aarch64::LaneCountFromFormat(VectorFormat)` |
| 83 | 1.0% | `_xzm_free` |
| 79 | 0.9% | `Memory::Write<uint8_t, uint64_t>` |
| 71 | 0.8% | `Simulator::AddSubHelper` |
| 67 | 0.8% | `Simulator::SimulateFPRoundInt` |
| 67 | 0.8% | `Simulator::VisitMoveWideImmediate` |
| 63 | 0.7% | `Simulator::VisitLogicalShifted` |
| 62 | 0.7% | `Simulator::MemWrite<uint64_t>` |
| 59 | 0.7% | `Simulator::LogicalHelper` |
| 57 | 0.7% | `vixl::RawbitsToFloat16(uint16_t)` |
| 57 | 0.7% | `Instruction::GetImmPCOffsetTarget` |
| 51 | 0.6% | `gaby_vm::Simulator::RunFrom(uintptr_t)` |

### Mixed Buckets

| Bucket | Samples | Percent |
|--------|--------:|--------:|
| NEON format helpers | 3925 | **45.8%** |
| `LogicVRegister::ClearForWrite` body | 1036 | 12.1% |
| `memset`, `memmove`, `__bzero`, mostly from `ClearForWrite` | 667 | 7.8% |
| Heap allocation and free for temporary register buffers | ~218 | ~2.5% |
| **Total NEON abstraction cost** | **5846** | **~68.2%** |
| `ExecuteInstructionCached` | 496 | 5.8% |
| `gaby_vm::Simulator::RunFrom` | 51 | 0.6% |
| `Visit*` entries | ~430 | ~5.0% |
| Leaf bodies | ~1700 | ~19.9% |
| Other stubs and helpers | ~120 | ~1.4% |

The `memset`/`memmove` samples mostly come from
`LogicVRegister::ClearForWrite`, which clears
`lane_size_in_bytes * lane_count`. Counting the function body plus its direct
memory-clear calls gives 1703 / 8566 samples, or about 19.9 percent of `mixed`.

Compared with the previous `mixed` profile, this run classifies more of the old
"leaf body" bucket as NEON abstraction. The qualitative conclusion is the same:
NEON helper overhead dominates.

## 3. Smoke Top-of-Stack Counts

| Samples | Percent | Function |
|--------:|--------:|----------|
| **2275** | **33.1%** | `vixl::aarch64::Simulator::ExecuteInstructionCached()` |
| **1243** | **18.1%** | `vixl::aarch64::Simulator::LogicalHelper` |
| **1207** | **17.5%** | `vixl::aarch64::Simulator::AddSubHelper` |
| 334 | 4.9% | `Simulator::VisitLogicalShifted` |
| 305 | 4.4% | `Simulator::VisitLoadStorePairPostIndex` |
| 298 | 4.3% | `gaby_vm::Simulator::RunFrom(uintptr_t)` |
| 276 | 4.0% | `Simulator::LoadStorePairHelper` |
| 268 | 3.9% | `mach_continuous_time` |
| 182 | 2.6% | `Simulator::AddWithCarry` |
| 103 | 1.5% | `Simulator::VisitUnconditionalBranchToRegister` |
| 46 | 0.7% | `Instruction::IsLoad` |
| 41 | 0.6% | `vixl::aarch64::CalcLSPairDataSize(LoadStorePairOp)` |
| 36 | 0.5% | `clock_gettime_nsec_np` |
| 25 | 0.4% | `mach_timebase_info` |
| 23 | 0.3% | `clock_gettime` |
| 18 | 0.3% | `gaby_vm::Simulator::Write(GpRegister, uint64_t)` |

Timer functions and the per-iteration LR reset belong to the benchmark harness,
not to simulator execution of the smoke body.

### Smoke Buckets

| Bucket | Samples | Percent |
|--------|--------:|--------:|
| **dispatch**, `ExecuteInstructionCached` plus `RunFrom` | 2573 | **37.4%** |
| **leaf helpers**, AddSub, Logical, LoadStorePair, AddWithCarry, IsLoad, size helpers | 2995 | **43.5%** |
| `Visit*` entries | ~750 | ~10.9% |
| **benchmark harness overhead**, timers plus LR write | 370 | **~5.4%** |
| Other stubs and cold-path functions | ~190 | ~2.8% |

Smoke observations:

1. `ExecuteInstructionCached` alone is 33 percent. This is the per-instruction
   prelude/postlude: PC state, current-range lookup, entry pointer computation,
   BTI and MOVPRFX checks, PMF call, `last_instr_`, `IncrementPc`,
   `LogAllWrittenRegisters`, and `UpdateBType`.
2. `AddSubHelper` plus `LogicalHelper` is 35.6 percent. The smoke body is mostly
   ALU instructions, and these helpers still extract fields from `Instruction`
   on every call.
3. `mach_continuous_time` at 3.9 percent is harness cost. `smoke` runs only
   32 instructions per `RunFrom`, so the timed loop calls the clock frequently.
4. NEON helpers have zero samples, which gives a clean control group for
   `mixed`-only NEON work.

## 4. Mixed vs Smoke Bottleneck Shape

```text
                        mixed%    smoke%
NEON abstraction          68%        0%
dispatch overhead          6%       37%
leaf helpers              20%       43%
Visit* entries             5%       11%
malloc/free                2%        0%
harness timer             <1%        5%
```

Read the table this way:

- `mixed` bottlenecks on NEON abstraction. Inlining NEON helpers, skipping
  unnecessary full-vector `ClearForWrite`, and removing heap temporary buffers
  can plausibly cut the 68 percent bucket by 50 to 60 percent.
- `smoke` bottlenecks on dispatch. NEON work does not help it.
- Leaf helper work helps both workloads, especially `smoke`.

## 5. Difference From the Previous Optimization Direction

The previous profile ranked the work as NEON helper inline, `ClearForWrite`
skips, then possible fast leaves pending a smoke profile. With smoke data:

- hot forms in smoke are concentrated enough that fast-form leaves would have a
  high hit rate;
- dispatch overhead is still the first smoke bottleneck, so fast leaves should
  be paired with dispatch shrinkage.

This reinforces the no-JIT conclusion from the previous profile. `smoke` is now
around 29 cycles/instruction. A pure interpreter floor is probably around
6 to 10 cycles/instruction. Beyond that, the project would need a different
route such as JIT or compile-time specialization.

## 6. 10x Goal Assessment

Using current cache-path baselines of 39.55 ns/instruction for `mixed` and
6.49 ns/instruction for `smoke`, a 10x target would require:

| Workload | Current | 10x target | Requirement |
|----------|--------:|-----------:|-------------|
| mixed | 39.55 ns | 3.95 ns | About 18 cycles/instruction; needs major NEON leaf rewrite, dispatch shrinkage, and likely JIT. |
| smoke | 6.49 ns | 0.65 ns | About 3 cycles/instruction, effectively native execution. |

Estimated realistic no-JIT ceilings:

- `mixed`: NEON helper inline, `ClearForWrite` gate, heap removal, and partial
  pre-extract could reach roughly 15 to 20 ns/instruction, about 2x to 2.6x.
- `smoke`: threaded dispatch, operand pre-extraction, fast-form leaves, and
  block dispatch could reach roughly 2.5 to 3.5 ns/instruction, about
  1.9x to 2.6x.

Under no-JIT constraints, 10x is not realistic. A 2x to 3x improvement is a
more realistic target range.

## 7. Data and Method Limits

- Single sampling run. The leading buckets are stable; tail entries under about
  20 samples are noisy.
- `mixed` is generated by VIXL and is not a real workload. Real app code likely
  has far less NEON than 68 percent.
- `memset` and `memmove` call sources were not split in this flat view. The
  full call graph is in the local `/tmp` dumps.
- Smoke's 5.4 percent harness overhead is the benchmark harness, not the
  simulator. Removing it would require a different timing window.

## 8. Index

- Previous throughput snapshot:
  `baseline-benchmark-results-cache-2026-05.md`.
- Previous mixed-only profile:
  `gaby-vm-cache-hotpath-profile-2026-05.md`.
- Cache path design:
  [`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md).
- Benchmark harness:
  [`bench/README.md`](../../bench/README.md).
- Current `ExecuteInstructionCached` implementation at the time of this note:
  `src/aarch64/simulator-aarch64.h:1496-1575`.
