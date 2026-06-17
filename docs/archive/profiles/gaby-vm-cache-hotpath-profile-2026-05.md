# Cache Hot-Path Profile, 2026-05

[Chinese version](gaby-vm-cache-hotpath-profile-2026-05.zh-cn.md)

This historical sampling profile examined the cache path on the `mixed`
workload after `predecode-cache-hotpath-speedup` landed at commit `ca7b580`.
It asked where the then-measured roughly 57 ns/instruction cache cost went.

Later measurements corrected the absolute 57 ns/instruction value downward for
steady-state cost, but the profile's percentage breakdown remained useful.

## Summary

In this `mixed` profile, about 56 percent of samples landed in NEON format
helpers and `LogicVRegister::ClearForWrite`. Dispatch itself was only about
5 percent.

| Category | Sample share | Notes |
|----------|-------------:|-------|
| NEON format helpers plus `ClearForWrite` | **~56%** | Fixed cost of VIXL's `LogicVRegister<VectorFormat>` abstraction. |
| `memset` / `memmove`, mostly from `ClearForWrite` | ~7% | Clears NEON destination storage. |
| `ExecuteInstructionCached` | ~5% | Full cache dispatch overhead on this workload. |
| `Visit*` and `*Helper` leaf bodies | ~32% | Load/store, AddSub, and NEON leaves. |

Conclusion at the time: for NEON-heavy `mixed`, the main lever was not dispatch
but repeated calls to externally-linked NEON format helper functions. Moving
pure `VectorFormat` helpers into headers as `constexpr inline` looked like the
highest-ROI next step.

## 1. Measurement Setup

- Machine: Apple Silicon arm64 on Darwin 25.5.0, no pinning.
- Build directory: `build/profile`.
- Flags: `-O3 -g -fno-omit-frame-pointer -DNDEBUG`.
- Workload: `bench_baseline --engine cache --seconds 15`.
- Tool: macOS `sample`, 1 ms interval, 10 seconds, 5573 samples.
- Output: local `/tmp/mixed_cache_profile.txt`.

Reproduction:

```bash
cmake -S . -B build/profile -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGABY_VM_BUILD_BENCHMARKS=ON \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -fno-omit-frame-pointer -DNDEBUG"
cmake --build build/profile --target bench_baseline

./build/profile/bench/bench_baseline --engine cache --seconds 15 &
sample $(pgrep -f bench_baseline | head -1) 10 -mayDie -file /tmp/profile.txt
```

## 2. Top-of-Stack Hotspots

Top entries from the flat view:

| Samples | Function |
|--------:|----------|
| **1380** | `vixl::aarch64::LaneSizeInBitsFromFormat(VectorFormat)` |
| **930** | `vixl::aarch64::IsSVEFormat(VectorFormat)` |
| **795** | `vixl::aarch64::LogicVRegister::ClearForWrite(VectorFormat)` |
| 309 | `vixl::aarch64::Simulator::ExecuteInstructionCached()` |
| 208 | `_platform_memset` |
| 199 | `_platform_memmove` |
| 129 | `Simulator::LoadStorePairHelper` |
| 117 | `Simulator::LoadStoreHelper` |
| 64 | `Memory::Write<uint8_t, uint64_t>` |
| 56 | `Simulator::FPPairedAcrossHelper<SimFloat16>` |
| 52 | `_xzm_free` |
| 48 | `Simulator::AddSubHelper` |
| **40** | `vixl::aarch64::LaneCountFromFormat(VectorFormat)` |

Malloc/free samples came from temporary `LogicVRegister` / `SimVRegister`
buffers in NEON leaves, so they belong to the same abstraction cost family.

## 3. Interpretation

The hottest helpers were ordinary `.cc` functions that switch on
`VectorFormat` and return lane size, lane count, or SVE/ASIMD classification.
Because they were externally linked, every leaf paid real calls instead of
letting the compiler fold constants.

NEON leaves call these helpers repeatedly to compute lane count, lane size, SVE
classification, and `ClearForWrite` sizes. The `mixed` workload had enough NEON
traffic to make those tiny helpers the dominant cost.

`ExecuteInstructionCached` at about 5.5 percent contained the entire cache
dispatch path: cache lookup, form hash write, PMF call, BType gate, last-instr
update, PC increment, and BType update.

## 4. What the Profile Corrected

Earlier guesses had overvalued operand extraction, register logging, and BType
bookkeeping. The profile showed:

- `Mask` / `Extract` were mostly inlined away and did not show as major flat
  samples.
- General-purpose register helpers were not the main cost.
- `LogAllWrittenRegisters` and `UpdateBType` did not appear as meaningful
  standalone cost.
- The dominant cost was specific to NEON's `VectorFormat` helper layer.

## 5. Optimization Implications at the Time

High ROI:

- Move `LaneSizeInBitsFromFormat`, `LaneCountFromFormat`,
  `IsSVEFormat`, and related helpers to `constexpr inline` header definitions.

Medium ROI:

- Investigate skipping `LogicVRegister::ClearForWrite` for full-vector writes.

Lower ROI than expected:

- Removing register log/update bookkeeping.
- Pre-extracting simple bitfields into `PredecodedEntry`.

Still missing at the time:

- A `smoke` profile without NEON, needed to expose pure dispatch and scalar leaf
  costs.

## 6. Native Slowdown Assessment at the Time

The original note estimated that pure interpreter work could reduce the
NEON-heavy `mixed` workload to roughly 80x to 120x slower than native, but not
to 30x without JIT or structural NEON leaf rewrites. Later benchmark docs
refined the denominator and target framing, but this note captured the first
evidence that NEON abstraction dominated `mixed`.

## 7. Limits

- Single sampling run, no median or variance.
- The generated `mixed` workload is not a real app workload and has a high NEON
  ratio.
- `memset` and `memmove` call sources were inferred from the flat profile; a
  deeper call tree would be needed for exact attribution.

## 8. Index

- Previous throughput snapshot:
  [`../benchmarks/baseline-benchmark-results-2026-05.md`](../benchmarks/baseline-benchmark-results-2026-05.md).
- Cache design:
  [`../../refs/gaby-vm-predecode-cache-design.md`](../../refs/gaby-vm-predecode-cache-design.md).
- Benchmark harness:
  [`../../../bench/README.md`](../../../bench/README.md).
- Cache hot-path optimization commit: `ca7b580`.
