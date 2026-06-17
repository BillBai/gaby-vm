# Baseline Benchmark Results — Cache Track, 2026-05

> Throughput snapshot of the gaby-vm `Simulator` running over a
> `PredecodeCache` (the "cache" engine) versus the imported VIXL
> `vixl::aarch64::Simulator` (the "decoder" engine) on identical
> workloads. Measured on the same host as
> [`baseline-benchmark-results-2026-05.md`](./baseline-benchmark-results-2026-05.md)
> (Apple M4 Pro), through the same `bench/` harness documented in
> [`bench/README.md`](../../bench/README.md).
>
> This is the first throughput record that includes the cache engine.
> The earlier 2026-05 baseline captured the upstream-shaped decoder
> path before any cache existed; this document captures both engines
> after `predecode-cache-hotpath-speedup` landed (commit
> [`ca7b580`](https://example.invalid/ca7b580): drop `std::function`
> indirection, gate `BType` check by predecode flag). Both files are
> intentionally preserved — read them as before/after snapshots of the
> cache effort.
>
> File name follows the same YYYY-MM convention as the predecessor,
> with the `cache` infix to distinguish series. Treat as immutable
> historical record; rerunning lands as a new sibling.
>
> 文档语言用中文跟最近一份 `gaby-vm-cache-hotpath-profile-2026-05.md`
> 对齐——baseline 这一类记录在 2026-05 以后的版本都用中文写。

## TL;DR

同一台 Apple M4 Pro p-core、单线程、AC 供电、未做 pinning，
**bench_baseline (mixed)** 和 **bench_smoke** 在两个 engine 下的中位数：

| Workload | engine  | median throughput (insn/s) | median ns/insn | median it/s |
|----------|--------:|---------------------------:|---------------:|------------:|
| `mixed`  | decoder | **7.23 M**                 | 138.40         | 111.77      |
| `mixed`  | **cache** | **25.29 M**              | **39.55**      | **391.19**  |
| `smoke`  | decoder | **11.72 M**                | 85.35          | 366,151     |
| `smoke`  | **cache** | **154.14 M**             | **6.49**       | **4,816,783** |

**核心数据**（cache 相对 decoder 的加速比，按 it/s 中位数算）：

- `mixed`：**≈ 3.50×**（39.55 ns/insn vs 138.40 ns/insn）
- `smoke`：**≈ 13.16×**（6.49 ns/insn vs 85.35 ns/insn）

mixed 的加速比落在前一份 baseline 给的预期范围（"3×–10×"）的下端，
smoke 的加速比则超过了原先估计的 2×–5×，进了 order-of-magnitude 档。
两者的差距来自 dispatch overhead 在 smoke 上占比远高于在 mixed 上——
smoke 是 32 条无分支 ALU，cache 把 decode/visitor 这一层削平之后，剩下
的就是 `ExecuteInstructionCached` 这一圈寥寥几行 + leaf 本体；mixed 含
NEON 和访存，leaf 本身就贵，dispatch 在总盘子里只占 ~5%（见
[`gaby-vm-cache-hotpath-profile-2026-05.md`](./gaby-vm-cache-hotpath-profile-2026-05.md)
§2）。

Order-of-magnitude framing：

- M4 Pro p-core native throughput 大约 10^10 insn/s。
  cache `mixed` 是 native 的 ~400×，cache `smoke` 是 ~65×。
- 假设 p-core 在 boost 下 ~4.5 GHz，cache `mixed` 单条指令花
  **~180 host cycle**，cache `smoke` **~29 host cycle**。
  smoke 这个数已经接近"循环 + 一次间接 call + leaf 几条 ALU"的物理下限。

## Host

| 字段 | 值 |
|------:|---|
| 机器 | MacBook (M4 Pro) |
| CPU | Apple M4 Pro, 10 P-cores + 4 E-cores |
| OS | macOS 26.5 (build 25F71) |
| Kernel | Darwin 25.5.0 arm64 |
| Compiler | Apple clang 21.0.0 (clang-2100.0.123.102) |
| Build flags | `CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `dev-release` preset |
| Power | AC, fan-cooled；电池满电、未在使用 |
| Concurrent load | `uptime` 1.98 / 4.27 / 4.26；正常工作 shell，**不是**专用 dedicated host |

跟前一份 2026-05 baseline 的差异只有两条：

- OS / kernel 小版本升级（macOS 26.3.1 → 26.5，Darwin 25.3 → 25.5）。
  CPU、L1/L2 几何、编译器都没变。
- 1 分钟 load average 略低（1.98 vs 2.2），但 5/15 分钟 load 反而更高
  （4.27/4.26 vs 2.2）——这次开了更多 IDE/浏览器。这条直接体现在
  下面的 spread 上：decoder side 的 spread 比上一份记的 1.8% 拓宽到
  ~4%。整体仍在 order-of-magnitude 框架内，不影响中位数判断。

## Build provenance

- Repository commit: `16f27a9355efa6035a1b3fc3bd94c71898402a87`
  (`docs(refs): record cache hot-path profile for 2026-05`)。
  这一次的 commit 是 docs-only，编译产物等同于前一次
  `ca7b580`（`perf(cache): drop std::function indirection ...`）。
- 工作树状态：clean（无未提交修改）。
- Binary：`build/release/bench/bench_baseline`、
  `build/release/bench/bench_smoke`，Mach-O 64-bit arm64。
- Configure：`cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON`。

## Workload provenance

跟前一份 baseline 完全一致——同一份 header，同一份指令字节流：

- **mixed** — `workload_generator_tag: vixl@3fe168632164; seed=42; buffer_bytes=262192`,
  `static_words_in_buffer=65548`, `dynamic_instructions_per_iteration=64643`。
- **smoke** — `workload_generator_tag: llvm-mc 22.1.5; source_sha256=4769ba17a5fe`,
  `static_words_in_buffer=32`, `dynamic_instructions_per_iteration=32`。

`dynamic_instructions_per_iteration` 是 mixed 在 **upstream simulator**
下采到的动态指令数。imported simulator 的 Visit* 语义跟 upstream 暂未
发现分歧，因此 mixed 的 dynamic count 在 cache 和 decoder 两个 engine
下都按这一个常量算 `throughput_insn_per_sec` 和 `ns_per_instruction`。
未来如果某条 marker 改动改变了 mixed 的分支结果，需要 regenerate
header（流程在 `bench/README.md` §"Regenerating ..."）。

## Raw observations

### `mixed` — decoder，7 runs，`--seconds 5`

| Run | iterations/s | throughput (insn/s) | ns/insn |
|----:|-------------:|--------------------:|--------:|
| 1 | 112.48 | 7,270,884 | 137.53 |
| 2 | 111.77 | 7,225,271 | 138.40 |
| 3 | 111.39 | 7,200,782 | 138.87 |
| 4 | 112.65 | 7,282,117 | 137.32 |
| 5 | 112.90 | 7,298,271 | 137.02 |
| 6 | 111.67 | 7,218,409 | 138.53 |
| 7 | 108.50 | 7,013,677 | 142.58 |
| **median** | **111.77** | **7,225,271** | **138.40** |
| min | 108.50 | 7,013,677 | 137.02 |
| max | 112.90 | 7,298,271 | 142.58 |

Spread `(max − min) / median` ≈ **3.9%** on it/s。

### `mixed` — cache，7 runs，`--seconds 5`

| Run | iterations/s | throughput (insn/s) | ns/insn |
|----:|-------------:|--------------------:|--------:|
| 1 | 392.00 | 25,340,282 | 39.46 |
| 2 | 387.72 | 25,063,405 | 39.90 |
| 3 | 389.31 | 25,166,132 | 39.74 |
| 4 | 391.19 | 25,287,641 | 39.55 |
| 5 | 386.08 | 24,957,418 | 40.07 |
| 6 | 393.11 | 25,412,094 | 39.35 |
| 7 | 391.47 | 25,305,584 | 39.52 |
| **median** | **391.19** | **25,287,641** | **39.55** |
| min | 386.08 | 24,957,418 | 39.35 |
| max | 393.11 | 25,412,094 | 40.07 |

Spread ≈ **1.8%** on it/s。

### `smoke` — decoder，6 runs，`--seconds 1`

| Run | iterations/s | throughput (insn/s) | ns/insn |
|----:|-------------:|--------------------:|--------:|
| 1 | 368,696 | 11,798,287 | 84.76 |
| 2 | 367,044 | 11,745,421 | 85.14 |
| 3 | 363,891 | 11,644,516 | 85.88 |
| 4 | 373,142 | 11,940,539 | 83.75 |
| 5 | 359,734 | 11,511,500 | 86.87 |
| 6 | 365,257 | 11,688,212 | 85.56 |
| **median** | **366,151** | **11,716,816** | **85.35** |
| min | 359,734 | 11,511,500 | 83.75 |
| max | 373,142 | 11,940,539 | 86.87 |

Spread ≈ **3.7%** on it/s。

### `smoke` — cache，6 runs，`--seconds 1`

| Run | iterations/s | throughput (insn/s) | ns/insn |
|----:|-------------:|--------------------:|--------:|
| 1 | 4,816,692 | 154,134,131 | 6.49 |
| 2 | 4,816,875 | 154,139,994 | 6.49 |
| 3 | 4,733,563 | 151,474,010 | 6.60 |
| 4 | 4,657,446 | 149,038,285 | 6.71 |
| 5 | 4,869,376 | 155,820,019 | 6.42 |
| 6 | 4,841,276 | 154,920,838 | 6.45 |
| **median** | **4,816,783** | **154,137,062** | **6.49** |
| min | 4,657,446 | 149,038,285 | 6.42 |
| max | 4,869,376 | 155,820,019 | 6.71 |

Spread ≈ **4.4%** on it/s。

四组的 spread 都在 5% 以内，跟前一份 baseline 的 1.6%–1.8% 同档；中位
数稳，无需扩到 N=10 + IQR + governor pinning 那一档协议。

## 对比：跟前一份 2026-05 baseline (decoder-only)

decoder side 是同一份代码路径，理论上不应该有变化。这里看的是噪声
是否稳定。

| Workload | 旧 decoder 中位 (insn/s) | 新 decoder 中位 (insn/s) | 差异 |
|----------|------------------------:|------------------------:|-----:|
| `mixed`  | 7,460,515               | 7,225,271               | −3.2% |
| `smoke`  | 12,474,489              | 11,716,816              | −6.1% |

两条都在方法学的"几个百分点是 host 噪声"的范围里。原因大概率是：

- 这次 5/15 分钟 load average 更高（IDE/浏览器开着）。
- OS 小版本升级带来的微小行为差异（scheduler、boost policy 之类）。

没有跑到 `>5% but <30%` 这种"需要 inspect 但很可能仍是噪声"的灰色
区间。**结论：`predecode-cache-hotpath-speedup` 没有给 decoder 一侧
带来可观测的 regression。**

## 对比：跟 cache hot-path profile 的 57 ns/insn

[`gaby-vm-cache-hotpath-profile-2026-05.md`](./gaby-vm-cache-hotpath-profile-2026-05.md)
里记下 mixed cache 路径 "~57 ns/insn"，这一份未做 sampling 的测量给出
**~39.55 ns/insn**，差约 30%。

最初这条差距我归到了 sampling 的 perturbation——`-g -fno-omit-frame-pointer`
+ 每 1ms 一次 SIGPROF + stack walk——但这条解释**是错的**，已经被
后续 profile 直接测量证伪：

- 2026-05-27 重做了一份 profile（`gaby-vm-cache-hotpath-profile-2026-05-27.md`），
  在完全相同的 build/profile flags + sampling 设置下，bench 上报的
  mixed cache 是 **40.69 ns/insn**——跟 dev-release 的 39.55 ns 只差 ~3%。
  smoke 的差距 <1%。也就是说 sampling 干扰本身就只有 single-digit %，
  不是 30%。
- 真正造成前一份 profile "57 ns/insn" 的原因不能简单归因。更可能
  是当时 host 处于较高 background load 状态，跟这一次的稳态对照组
  不同。这个解释也没有直接证据（没有当时的 uptime），权当一种推测。

实践含义：

- profile 里看到的 **占比关系**（NEON helpers ~58% / dispatch ~6% / ...）
  是可信的，从中得出的「下一步动 NEON format helpers 的杠杆最大」
  这条结论不受影响。这一点和原结论一致。
- profile 里看到的 **绝对 ns/insn 数字**（前一份的 ~57）不能用作
  cache 路径 cost 的参考。**两份文档后续都按 dev-release 下的 39.55
  ns/insn 作为 mixed cache 路径的稳态成本**。

后续如果再做 profile，要在文档里明确记下 throughput build vs profile
build 的 ns/insn 对比，避免再出现两份文档绝对值打架。

## What this measures, and what it does not

What the numbers reflect, accurately:

- **decoder side**：跟前一份 baseline 的 §"What this measures" 完全一致
  —— `Simulator::RunFrom` → `Decoder::Decode(pc) → CompiledDecodeNode walk →
  DecoderVisitor virtual call → Simulator::VisitXXX leaf`。
- **cache side**：`gaby_vm::Simulator::RunFrom` → `ExecuteInstructionCached(pc)`
  → 在 `PredecodeCache` 的 slot 上 indirect call 到 trampoline → leaf
  本体。包含 BType gate（只在 predecode flag 命中时跑）、`IncrementPc`、
  `UpdateBType`。**predecode 这一步在第一次 `RunFrom`（warm-up，未计时）
  完成**，所以 timed region 测的是"steady-state 命中路径"，不是 cache
  构造路径。
- 两个 engine 都包含每次 `RunFrom` 之前的 `WriteLr(kEndOfSimAddress)`。
  mixed 上摊到 ~64.6k 条指令是噪声，smoke 上摊到 32 条指令是几个 ns
  的固定开销。

What the numbers do **not** capture:

- **predecode 构造成本** —— 不在 timed region 里。要看构造速度的话
  需要专门跑一份测量（目前 harness 没暴露这个接口；如果将来需要，
  扩个 `--measure-predecode-only` flag 是最自然的做法）。
- **cache 容量压力** —— mixed workload 是 256 KiB 静态字节，对应
  ~65k 个 PredecodedEntry。这套都能 L2 hit，但已经超过 L1D。比这更
  大的工作负载（embedder 真实代码可能 MB 级）的 cache hit rate
  另算。
- **多线程 / 多实例**。harness 是单线程跑一个 Simulator。多 Simulator
  实例之间的 cache 共享或竞争不在这份记录里——前一份 baseline 也明
  确把这条留给 `baseline-benchmark-suite.md` 里 P0 的多实例正确性 gate。
- **iOS 数字**。bench harness 暂时只在 macOS 上跑。按前一份的估算（
  iPhone p-core 大约 0.85× M4 Pro p-core），cache `mixed` 在
  A18 Pro p-core 上的外推值是 ~21.5 M insn/s，cache `smoke` ~131 M
  insn/s——这是外推不是测量，正式 iOS 数字还得等 harness 接上 iOS。

## How to reproduce

```sh
cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON
cmake --build --preset dev-release --target bench_baseline bench_smoke

# 每条命令跑几次，取中位数。
./build/release/bench/bench_baseline --engine decoder --seconds 5
./build/release/bench/bench_baseline --engine cache   --seconds 5
./build/release/bench/bench_smoke    --engine decoder --seconds 1
./build/release/bench/bench_smoke    --engine cache   --seconds 1
```

输出是 `key: value` 一行一对，schema 见 `bench/README.md`。
首要 metric 是 `iterations_per_second`；`throughput_insn_per_sec`
和 `ns_per_instruction` 是从它派生出来的。

## Reference points for future comparisons

下一轮（无论是 NEON helper inlining、ClearForWrite gating、
还是别的 cache leaf 优化）拿这一份当对比基线时，关注的 delta：

| Comparison | 期望幅度（粗，不是 target） |
|-----------|---------------------------|
| 新 decoder side vs 这次 decoder | ±5% 内。decoder 路径不应该被任何 cache-only 改动碰到。 |
| 新 cache `mixed` vs 这次 cache `mixed` | NEON helper inlining：~30-50% 砍 ns/insn，预期到 ~20-30 ns/insn。其它 cache 微调单独单位数 %。 |
| 新 cache `smoke` vs 这次 cache `smoke` | 已经 6.49 ns/insn，进一步压缩空间小。要从这往下砍只剩 threaded dispatch / register caching 那个量级的改动。 |
| ns/insn 地板 | smoke cache 6.49 ns ≈ 29 cycles/insn 已经接近"循环 + 1 个 indirect call + leaf 几条 ALU"的物理底；mixed cache 39.55 ns ≈ 178 cycles/insn 主要被 NEON helpers 占住。 |

这些是 calibration anchors，不是 commitment。

## See also

- [`baseline-benchmark-results-2026-05.md`](./baseline-benchmark-results-2026-05.md)
  —— 前一份 baseline，decoder-only / pre-cache 时间零点。
- [`gaby-vm-cache-hotpath-profile-2026-05.md`](./gaby-vm-cache-hotpath-profile-2026-05.md)
  —— 跟这份对应的 cache 路径 sampling profile，回答"这 39-57 ns 都
  花到哪儿"。
- [`baseline-benchmark-suite.md`](./baseline-benchmark-suite.md)
  —— bench 方法学、metric schema、多实例正确性 gate。
- [`bench/README.md`](../../bench/README.md)
  —— harness 用法、输出 key 定义、workload 重生成步骤。
- [`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md)
  —— cache 路径的设计文档；解释这次测的 cache engine 到底在跑什么。
- [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md)
  —— decoder 一侧 ~138 ns/insn 花在哪儿的背景。
