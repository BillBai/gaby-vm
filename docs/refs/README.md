# docs/refs — 参考资料索引

这里放 gaby-vm 的**长青参考资料**：VIXL 背景知识、当前生效的设计文档、以及仍被引用的
基线/profile 快照。一次性的历史快照和被取代的旧稿不留在这里——见文末「归档」。

## 怎么用这个目录

- 找东西先看下面的分类，每个文件都标了它是干嘛的、谁在引用它。
- **标了「锚点」的文件别移动、别改名**：它们被 shipping 代码注释或 openspec spec 按路径
  （甚至按 `§` 章节）引用，挪了会让那些引用失效。新的历史快照请直接放进
  [`../archive/`](../archive/)，不要在这里堆积。
- **按旧路径找不到某个文件？** 它多半被归档了。直接按文件名全局搜索
  （`git ls-files | grep <名字>`，或编辑器全局搜索）——文件名是稳定的，搜索总能找到。
  历史文档会定期归档，我们不再维护那些指向归档文件的精确路径。

## VIXL 背景知识

给贡献者的 VIXL 入门与事实参考（外部知识，长青）。

- `vixl-overview.md` — VIXL 项目总览。
- `vixl-aarch64-simulator-architecture.md` — AArch64 Simulator 架构：寄存器、执行模型总览。
- `vixl-decode-dispatch-pattern.md` — 每条指令的 decode/dispatch 控制流，事实参考。
- `vixl-fetch-decode-dispatch-deep-dive.md` — fetch/decode/dispatch 深度分析 + 预解码缓存
  的由来。（带 `.html` 可视化副本）

## 导入边界

- `vixl-extraction-map.md` — **锚点**。VIXL 导入 Tier 清单、Tier-0 边界、pinned 导入 SHA。
  被 `AGENTS.md`、`architecture.md`、`testing.md`、`onboarding.md` 和两处 CMake 大量引用，
  是引用最密的稳定锚点。

## gaby-vm 设计（当前生效）

- `gaby-vm-predecode-cache-design.md` — **锚点**。预解码/分发缓存的权威设计。被 shipping
  代码注释（`predecode_cache.h`、`simulator-aarch64.h`）按 `§` 章节引用。
- `gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md` — 当前 `vixl_port` 套件（现场
  汇编 + 两轨差分）的设计。被 `testing.md` 引。
- `gaby-vm-predecode-cache-data-in-stream-2026-06-10.md` — data-in-stream 支持设计
  （进行中的 change）。被 `testing.md` 与 openspec change 引。（带 `.html` 可视化副本）

## 基线与 profile 快照（仍被引用）

这些虽是某个时点的快照，但还被活的代码/文档/spec 引用，所以留在这里。

- `baseline-benchmark-suite.md` — 基准套件方法论与索引。被 openspec `benchmark-harness`
  spec 引。
- `gaby-vm-business-bench-2026-06-08.md` — 业务基准 `bench_business` 首份数据与解读。被
  `bench/README.md` 引。
- `gaby-vm-business-bench-applogic-2026-06-09.md` — 给业务基准补上 FP/NEON（`applogic`
  核）。被 `bench/README.md` 引。
- `gaby-vm-cache-hotpath-profile-2026-05-27.md` — **锚点**。cache 热路径采样 profile。被
  shipping 代码注释 + `aarch64-simulator` spec 引。

## 归档

历史/过时的文档会定期移到 [`../archive/`](../archive/)：

- `archive/benchmarks/` — 早期 baseline 结果快照。
- `archive/profiles/` — 早期 cache 热路径 profile。
- `archive/superseded/` — 被取代的设计稿、计划稿、过程记录、早期草图。

旧路径的引用（尤其 `openspec/changes/archive/` 里的）不会逐个回改——按文件名搜索即可定位。
