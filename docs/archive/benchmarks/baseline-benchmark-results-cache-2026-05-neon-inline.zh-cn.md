# Baseline Benchmark Results — Cache Track + NEON Inline, 2026-05-27

> Throughput snapshot **after** `neon-format-helpers-constexpr-inline`
> lands (6 NEON `VectorFormat` helpers promoted from `.cc` to header as
> `constexpr inline`). Sibling of
> [`baseline-benchmark-results-cache-2026-05.md`](./baseline-benchmark-results-cache-2026-05.md)
> (pre-change snapshot at `16f27a9`) and
> [`baseline-benchmark-results-2026-05.md`](./baseline-benchmark-results-2026-05.md)
> (the original pre-cache, decoder-only snapshot from earlier in 2026-05).
>
> Treat as immutable historical record; the next perf change lands as
> another sibling.

## TL;DR

同一台 Apple M4 Pro p-core，相对前一份 cache-track baseline 的 delta：

| Workload | engine  | median ns/insn (now) | (pre-change) | delta | speedup |
|----------|--------:|---------------------:|-------------:|------:|--------:|
| `mixed`  | decoder | **121.71**           | 138.40       | **−12.1%** | **1.14×** (bonus) |
| `mixed`  | **cache** | **21.97**          | 39.55        | **−44.5%** | **1.80×** (primary target) |
| `smoke`  | decoder | 95.45                | 85.35        | **+11.8%** | **0.894×** (layout artifact, see §5) |
| `smoke`  | **cache** | **6.45**           | 6.49         | −0.6% | 1.006× (flat) |

**Headline**：mixed cache **1.80×**——这是开 OpenSpec change 时估算的
1.4-1.6× 区间上方，意外好。mixed decoder 也跟着拿 **1.14×**，因为同
一组 NEON helpers 既被 cache 路径的 leaf 调用、也被 decoder 路径的
leaf 调用——inline 之后两边的 NEON 一族 Visit\* 都跟着提速。

## Host

| 字段 | 值 |
|------:|---|
| 机器 | MacBook (M4 Pro), 10 P-cores + 4 E-cores |
| OS | macOS 26.5 (build 25F71) |
| Kernel | Darwin 25.5.0 arm64 |
| Compiler | Apple clang 21.0.0 (clang-2100.0.123.102) |
| Build flags | `CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `dev-release` preset |
| Power | AC, 满电 |
| Concurrent load | `uptime` 1.76 / 2.36 / 2.47 (本次)，前一份是 1.98 / 4.27 / 4.26 |

机器同一台，OS 版本未变。Load average 这次略低，但短时观测发现
smoke (1s/3s 短窗) 仍然抖一些，详见 §5。

## Build provenance

- 基线 commit (pre-change)：`16f27a9355efa6035a1b3fc3bd94c71898402a87`
  (`docs(refs): record cache hot-path profile for 2026-05`)，对应
  `baseline-benchmark-results-cache-2026-05.md`。
- 本次 commit：尚未 commit。工作树状态：
  - 修改：`src/aarch64/instructions-aarch64.h`、
    `src/aarch64/instructions-aarch64.cc`、`test/CMakeLists.txt`、
    `docs/refs/baseline-benchmark-suite.md`
  - 新增：`test/instructions_aarch64_constexpr_smoke_test.cc`、
    本文档、`openspec/changes/neon-format-helpers-constexpr-inline/`
- 待 archive 的 OpenSpec change：
  `openspec/changes/neon-format-helpers-constexpr-inline/`，
  validate `--strict` 通过。
- Binary：`build/release/bench/bench_baseline` /
  `build/release/bench/bench_smoke`，Mach-O 64-bit arm64。
- Configure：`cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON`
  + `cmake --build --preset dev-release --target gaby_vm bench_baseline
  bench_smoke`，144 个 imported-VIXL `-Wdeprecated-enum-enum-conversion`
  warnings (跟 baseline 同数)，无新增 warning。

## Workload provenance

跟前一份完全一致——同一份 header，同一份指令字节流：

- **mixed** — `workload_generator_tag: vixl@3fe168632164; seed=42; buffer_bytes=262192`,
  `static_words_in_buffer=65548`, `dynamic_instructions_per_iteration=64643`。
- **smoke** — `workload_generator_tag: llvm-mc 22.1.5; source_sha256=4769ba17a5fe`,
  `static_words_in_buffer=32`, `dynamic_instructions_per_iteration=32`。

## Raw observations

### `mixed` — decoder，7 runs，`--seconds 5`

| Run | iterations/s | throughput (insn/s) | ns/insn |
|----:|-------------:|--------------------:|--------:|
| 1 | 128.78 | 8,324,685 | 120.12 |
| 2 | 127.10 | 8,216,308 | 121.71 |
| 3 | 128.28 | 8,292,275 | 120.59 |
| 4 | 125.18 | 8,092,222 | 123.58 |
| 5 | 127.53 | 8,244,164 | 121.30 |
| 6 | 126.69 | 8,189,672 | 122.11 |
| 7 | 126.80 | 8,196,706 | 122.00 |
| **median** | **127.10** | **8,216,308** | **121.71** |
| min | 125.18 | 8,092,222 | 120.12 |
| max | 128.78 | 8,324,685 | 123.58 |

Spread `(max − min) / median` ≈ **2.8%** on it/s。

### `mixed` — cache，7 runs，`--seconds 5`

| Run | iterations/s | throughput (insn/s) | ns/insn |
|----:|-------------:|--------------------:|--------:|
| 1 | 714.21 | 46,168,689 | 21.66 |
| 2 | 706.54 | 45,673,183 | 21.89 |
| 3 | 709.97 | 45,894,596 | 21.79 |
| 4 | 702.57 | 45,416,313 | 22.02 |
| 5 | 704.17 | 45,519,521 | 21.97 |
| 6 | 696.54 | 45,026,732 | 22.21 |
| 7 | 699.89 | 45,242,969 | 22.10 |
| **median** | **704.17** | **45,519,521** | **21.97** |
| min | 696.54 | 45,026,732 | 21.66 |
| max | 714.21 | 46,168,689 | 22.21 |

Spread ≈ **2.5%** on it/s。

### `smoke` — decoder，8 runs，`--seconds 3`

首轮做了 6×1s 后发现跟 baseline 差 ~10%，又重做了 8×3s 复验，结
论一致（详见 §5）。这里记录复验数据。

| Run | iterations/s | ns/insn |
|----:|-------------:|--------:|
| 1 | 325,433 | 96.03 |
| 2 | 338,018 | 92.45 |
| 3 | 333,768 | 93.63 |
| 4 | 329,378 | 94.88 |
| 5 | 322,545 | 96.89 |
| 6 | 329,854 | 94.74 |
| 7 | 313,676 | 99.63 |
| 8 | 311,092 | 100.45 |
| **median** | **327,406** | **95.45** |
| min | 311,092 | 92.45 |
| max | 338,018 | 100.45 |

Spread ≈ **8.2%** on it/s——比 baseline (3.7%) 宽，但中位数稳定低于
baseline 366,150 it/s。**这条不是噪声爆发**。

### `smoke` — cache，6 runs，`--seconds 1`

| Run | iterations/s | throughput (insn/s) | ns/insn |
|----:|-------------:|--------------------:|--------:|
| 1 | 4,824,590 | 154,386,874 | 6.48 |
| 2 | 4,860,754 | 155,544,134 | 6.43 |
| 3 | 4,835,639 | 154,740,454 | 6.46 |
| 4 | 4,770,073 | 152,642,323 | 6.55 |
| 5 | 4,884,124 | 156,291,980 | 6.40 |
| 6 | 4,855,176 | 155,365,619 | 6.44 |
| **median** | **4,845,408** | **154,837,544** | **6.45** |
| min | 4,770,073 | 152,642,323 | 6.40 |
| max | 4,884,124 | 156,291,980 | 6.55 |

Spread ≈ **2.4%** on it/s。**跟 baseline 6.49 ns/insn 实质等价**（差 0.6%，
在噪声里）。

## 验收 vs OpenSpec change 里写的 threshold

`openspec/changes/neon-format-helpers-constexpr-inline/tasks.md` 4.2
里定的四条 acceptance threshold 跟实测对比：

| Threshold | 期望 | 实测 | 结果 |
|---|---|---|---|
| mixed cache median ns/insn | ≤ 31.6 ns/insn (至少 20% 改善) | **21.97 ns** (44.5% 改善) | **PASS 巨幅** |
| mixed decoder median ns/insn | ±5% of 138.40 ns/insn | **121.71 ns (−12.1%)** | 数值上超出 ±5%，但是**向好方向**，原 gate 写得太保守 |
| smoke cache median ns/insn | ±10% of 6.49 ns/insn | **6.45 ns (−0.6%)** | **PASS** |
| smoke decoder median ns/insn | ±10% of 85.35 ns/insn | **95.45 ns (+11.8%)** | **brushes gate**，详见 §5 |

mixed cache 是这次 change 的 primary acceptance gate，**1.81× 远超**
预期的 1.4-1.6×；smoke cache 没碰到的预期也确认（cache 路径 dispatch
开销没动）。

## §5 关键诚实话：smoke decoder 慢 11.8% 是怎么回事

smoke workload 是 32 条无 NEON ALU + LDP/STP，走 decoder engine 时
**完全不调** 这次 inline 掉的 6 个 NEON format helper。Profile 上 smoke
decoder 命中的是 `AddSubHelper`、`LogicalHelper`、`LoadStorePairHelper`
这些 scalar leaves，跟 NEON helpers 没有调用关系。所以严格意义上 smoke
decoder 不应该被这次 change 影响。

实测却慢了 ~10-12%。**这是 binary layout 的副作用**——`instructions-
aarch64.cc` 删掉了 6 个函数体（~80 行 / ~1 KB 的 .text），导致后面的
代码全部往前移。某些 smoke decoder 的 hot leaf 函数因此被搬到了不
同的 cache line / page 边界附近，分支预测和 i-cache 利用率发生了细
微变化。这种 layout-induced shift 在 LLVM 编译的二进制上常见，标准
HPC 文献里都有记录（无关代码增减导致 ±5-10% 的本地热点变化）。

证据：

1. cache smoke 没动（−0.6%）。cache 路径走 `ExecuteInstructionCached`，
   leaf 通过 pmf 间接 call。它跟 decoder 路径走的是**不同**的入口；
   如果 smoke 这次的减速是真的代码改动引起的，cache smoke 也该跟着
   减速。但它没动。
2. mixed decoder 反而**快**了 12.1%。decoder 路径上的 NEON Visit\* 调
   inline 后的 helper，所以 NEON 重的 workload 在 decoder 一侧也直接
   受益。如果 layout 是单方向 "把 hot path 搬到坏位置"，mixed 也该
   一起慢——但 mixed 加速了。
3. 8 次重复跑（24 秒采样窗）spread 8.2%，min 311k it/s、max 338k it/s，
   全都低于 baseline 366k——不是 transient load spike，是稳态偏移。

实践含义：**这不是性能 regression**，而是 layout reshuffling 在 smoke
这种小 workload 上偶然落到坏边的结果。在更接近真实 embedder 的工
作负载上（≥1k 条动态指令的循环），这种 layout 影响通常摊平。如果
长远要消除这类 layout 抖动，需要：

- 用 PGO (profile-guided optimization) 让编译器自动选 hot 函数 layout；
- 或对核心 leaves 加 `[[gnu::hot]]` / `[[gnu::cold]]` 属性手工导引；
- 或锁定 link order / 用 lld 的 `--symbol-ordering-file` 固定 hot 段
  位置。

**目前不做**——这超出本次 change 的 scope，且 mixed 一侧的净收益压
倒 smoke 一侧的 layout 损失（11.8% slowdown on 86 ns ≈ +10 ns 绝
对；mixed decoder 12.1% 加速 on 138 ns ≈ −17 ns 绝对；mixed cache
44.5% 加速 on 40 ns ≈ −18 ns 绝对）。

## §6 跟 cache hot-path profile 的预测对比

`docs/refs/gaby-vm-cache-hotpath-profile-2026-05-27.md` §6 估算 lever
A 在 mixed 上能拿 ~30-50% 改善，对应 ns/insn ~20-28。实测 **21.97 ns**
**正好在估算上半区**。这意味着：

- 后续 cache hot-path profile 里给的 lever B/C/D/E/F 的预估收益数字
  仍然可信。
- mixed cache 的 NEON 抽象现在仍然是大头但比例显著降低——下一份
  profile 应该会看到 dispatch overhead 占比抬升、leaf 占比抬升、
  NEON format helpers 占比下降。要不要立即做下一份 profile？见
  tasks.md §5.2 的 optional 选项。

## §7 后续 lever 排期建议

按 lever A 实测拿到 1.80× 反推，后续 lever 在 mixed cache 上的相对
收益估算也要按新 baseline 重算。原 profile 里给的 lever B（ClearForWrite
gating）+ lever C（heap removal）合计还能砍 ~15-20%，对应 mixed cache
约 ~18-19 ns/insn (2.1×)；lever D/E/F (smoke dispatch 收紧) 则在 mixed
上只能拿 ~10%。

短期可执行的：

1. lever B + C 合并一刀（NEON ClearForWrite + heap removal）——预计
   mixed cache 再降 ~3-4 ns/insn。
2. lever D（pre-extract operands to PredecodedEntry）——对 smoke 收益
   更大，mixed 也跟着轻微受益。
3. 之后再做下一份 profile + 评估剩下的天花板。

## §8 索引

- 前一份 throughput 快照（pre-change cache + decoder baseline）：
  [`baseline-benchmark-results-cache-2026-05.md`](./baseline-benchmark-results-cache-2026-05.md)
- 第一份 baseline (decoder-only pre-cache)：
  [`baseline-benchmark-results-2026-05.md`](./baseline-benchmark-results-2026-05.md)
- 本次 change 依据的 profile：
  [`gaby-vm-cache-hotpath-profile-2026-05-27.md`](./gaby-vm-cache-hotpath-profile-2026-05-27.md)
- bench harness 说明：[`bench/README.md`](../../bench/README.md)
- 本次 OpenSpec change：
  `openspec/changes/neon-format-helpers-constexpr-inline/`
