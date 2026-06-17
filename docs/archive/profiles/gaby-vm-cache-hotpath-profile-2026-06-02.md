# Cache Hot-Path Profile and Optimization Roadmap, 2026-06-02

[Chinese version](gaby-vm-cache-hotpath-profile-2026-06-02.zh-cn.md)

This archived profile followed the 2026-05 cache-path profiles after several
NEON/cache optimizations had landed. It added a native denominator and used the
new measurements to sketch an optimization roadmap. The note is historical; the
active benchmark and profile docs under `docs/refs/` supersede parts of the
target framing.

## Summary

Three key changes versus the earlier profiles:

1. The `mixed` bottleneck changed. The earlier top NEON format helpers had been
   inlined or made constexpr, and `mixed` cache dropped from about
   39.5 ns/instruction to about 19.6 ns/instruction. The new top `mixed` cost
   became NEON temporary-object heap allocation plus `memcpy`/`memset`, around
   26 percent.
2. The benchmark added a native denominator through `native_baseline` and
   `native_smoke`, which run the same committed workload bytes directly on an
   arm64 host CPU.
3. `ExecuteInstructionCached` became the top single function in both workloads:
   about 39 percent of `smoke` and 12 percent of `mixed`.

Throughput at the time:

- `mixed` cache: about 19.6 ns/instruction, roughly 359x versus native.
- `smoke` cache: about 6.45 ns/instruction, roughly 113x versus native.

The note concluded that a 50x native target had very different difficulty by
workload. `smoke` could plausibly reach it under no-JIT constraints. The
NEON-heavy `mixed` workload could not reach it without much larger NEON/FP leaf
work.

## 1. Measurement Setup

- Code state: commit `035f20b`, including cache and NEON optimizations through
  `branch-hook-api`.
- Machine: Apple M4 Pro, macOS 26.5, no pinning, P-core roughly 4.4 GHz.
- Sampling: macOS `sample`, 1 ms interval.
- Profile build: `-O3 -g -fno-omit-frame-pointer -DNDEBUG`.
- Native baseline: `bench/native_baseline` and `bench/native_smoke` run the same
  committed workload bytes directly on arm64 host CPU.

The profile build was about 3 percent slower than dev-release, matching the
2026-05-27 observation.

## 2. Native Target Breakdown

The original note translated "50x native" into per-instruction budgets:

| Workload | cache at the time | 50x target | required speedup | current cycles/insn | target cycles/insn |
|----------|------------------:|-----------:|-----------------:|--------------------:|-------------------:|
| `mixed` | ~19.6 ns | ~2.7 ns | ~7.2x | ~86 | ~12 |
| `smoke` | ~6.45 ns | ~2.86 ns | ~2.26x | ~28 | ~13 |

The key interpretation was that the same "50x" target is much harder for a
NEON-heavy workload because native executes a vector instruction as one hardware
instruction while the interpreter expands it into C++ lane work.

The note later added an important recalibration: native slowdown is a proxy, not
the final product target. A more useful user-facing comparison is against
embeddable interpreters such as Lua or JavaScriptCore LLInt. Later business
benchmarks refined this framing.

## 3. Fresh `mixed` Profile

7851 samples, bucketed:

| Bucket | Samples | Percent | Meaning |
|--------|--------:|--------:|---------|
| NEON temporary-object memory management | 2024 | **25.8%** | Heap allocation plus `memcpy`/`memset` for temporary `LogicVRegister` / `SimVRegister` buffers. |
| Dispatch, `ExecuteInstructionCached` + `RunFrom` | 1032 | **13.1%** | Fixed per-instruction tax, now the top single function. |
| `Visit*` entries | ~750 | ~9.5% | Leaf entry layer. |
| NEON/FP/format residuals | ~640 | ~8.2% | Remaining format and lane costs after inline/constexpr work. |
| Load/store leaves | ~16% | - | Imported VIXL leaf work. |

Compared with the 2026-05-27 profile:

| Category | Previous | This profile | Change |
|----------|---------:|-------------:|--------|
| NEON format helpers | ~45.8% | ~8% residual | Inlining removed the old top bucket. |
| NEON temp heap + memory ops | ~10% | ~25.8% | Became visible as denominator shrank. |
| Dispatch | ~6.4% | ~13.1% | Relative share rose. |
| Leaf helpers | ~20% | ~21% | Roughly unchanged. |

## 4. Fresh `smoke` Profile

5351 samples, with harness timer counted separately:

| Bucket | Samples | Percent | Meaning |
|--------|--------:|--------:|---------|
| Dispatch, `ExecuteInstructionCached` + `RunFrom` | 2399 | **44.8%** | Fixed per-instruction tax. |
| ALU leaves, `LogicalHelper`, `AddSubHelper`, `AddWithCarry` | 2380 | **44.5%** | Repeated operand extraction inside scalar leaves. |
| `Visit*` entries | ~752 | ~14.1% | Leaf entry layer. |
| Benchmark timer | ~310 | ~5.8% | Harness cost, not simulator cost. |

`ExecuteInstructionCached` included inlined bookkeeping such as `IncrementPc`,
`UpdateBType`, MOVPRFX checks, `form_hash_` writes, and PMF loading. The note
confirmed that `LogAllWrittenRegisters` was not secretly walking registers when
trace was off.

## 5. Roadmap From This Snapshot

The roadmap split work into phases, with separate OpenSpec changes and
measurement after each one:

### Phase 1: Dispatch Shrink

Targets:

- replace virtual PMF dispatch with a thunk table where possible;
- gate MOVPRFX checks behind an SVE-relevant flag bit;
- tighten the block-dispatch inner loop so `RunFrom` does less per-step
  wrapper work;
- consider threaded dispatch as a larger follow-up.

Expected result at the time: meaningful `smoke` gains, smaller `mixed` gains.

### Phase 2: Scalar Operand Work

Specialize hot scalar forms and reduce repeated extraction in ALU leaves,
especially for `smoke`.

### Phase 3: NEON Temporary Object Lifetime

Attack `mixed`'s new top bucket by reducing temporary `LogicVRegister` /
`SimVRegister` heap and memory-copy cost.

### Phase 4: Larger NEON/FP Leaf Rewrite

A high-risk path for the NEON-heavy workload: use host SIMD intrinsics or deeper
leaf rewrites. The note treated this as out of scope for small cache-path work.

## 6. Decision Recorded

The note recorded three scheduling decisions:

- keep both `mixed` and `smoke` because they expose different bottlenecks;
- keep the strict no-JIT target;
- accept workload-specific outcomes rather than pretending one speedup number
  describes all workloads.

## 7. Limits

- Single-host measurements with normal development-machine noise.
- `mixed` is generated and NEON-heavy, not a real app trace.
- The native denominator overstates user-facing slowdown for some business
  logic shapes, which later `bench_business` work addressed.

## 8. Index

- Earlier profiles:
  [`gaby-vm-cache-hotpath-profile-2026-05.md`](./gaby-vm-cache-hotpath-profile-2026-05.md),
  [`../../refs/gaby-vm-cache-hotpath-profile-2026-05-27.md`](../../refs/gaby-vm-cache-hotpath-profile-2026-05-27.md).
- Cache design:
  [`../../refs/gaby-vm-predecode-cache-design.md`](../../refs/gaby-vm-predecode-cache-design.md).
- Benchmark harness:
  [`../../../bench/README.md`](../../../bench/README.md).
