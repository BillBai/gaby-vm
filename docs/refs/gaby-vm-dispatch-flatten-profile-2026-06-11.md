# Dispatch-Flatten Profile & Design Analysis — 2026-06-11

> 针对**业务核标量场景**（`bench_business` 的 `parse` / `fsm`）对 cache 路径做的
> 一次定向 sampling profile，外加由数据带出来的一组设计判断：二次派发 + 虚调用到底
> 占多少、movprfx 检查实测代价、operand 怎么存、以及 flatten 之后能不能彻底扔掉
> `form_hash`。
>
> 跟前一份 [`gaby-vm-cache-hotpath-profile-2026-05-27.md`](./gaby-vm-cache-hotpath-profile-2026-05-27.md)
> 的关系：那份采的是 smoke（纯 ALU 极限态）和 mixed（NEON 抽象代价主导）；这份采的是
> **真实 iOS 业务逻辑形状**的 `parse`（branch-dense 解析）和 `fsm`（per-byte 状态机，
> 解释器最差场景）。结论方向一致（dispatch + leaves 主导、NEON 不参与），但这份把
> 「dispatch 那一坨到底是什么」拆得更细，并且**实测**了 movprfx。
>
> 方法学跟 05-27 那份基本同源，但 build flag 略有不同，见下。

## 方法

- `build/profile` 重配为 **`RelWithDebInfo`**（`-O2 -g -DNDEBUG` + `-fno-omit-frame-pointer`）。
  注意跟 05-27 那份的 `-O3 -g` 不同——选 `-O2` 是因为它对 leaf 的内联没 `-O3` 激进，
  函数级归因更干净；而 dispatch 走的是**间接调用**（虚 pmf），leaf 本来就不会被内联进
  driver 循环，所以两者在采样里天然分帧。
- macOS `/usr/bin/sample`，间隔 1 ms，每核采 12 s，约 **1.03 万采样/核**。
- 该 build 的 ns/insn（`parse` 6.97 / `fsm` 6.67）跟 `-O3` headline（~6.5）差 **<7%**，
  所以下面的百分比可以直接迁到生产数。
- 代码状态：commit `e670814`（data-in-stream sentinel slots 落地之后）。

## TL;DR

- **dispatch hub 是最大单桶**：`parse` 38.6% / `fsm` 42.1%（越 branchy 越高，因为 leaf 越短，
  每条指令的固定开销越显眼）。但这一坨**大头不是虚调用**，是固定的每指令记账：range 检查、
  `form_hash_` 写、movprfx 双比较、PC/BType 推进。
- **字面意义的「二次派发 + virtual」其实很小，~5–10%**。真正值得 flatten 的原因是**结构性**的：
  消掉 `Visit*→Helper` 的调用分层和跨层重复抽取，而不是删掉那个 switch 本身。
- **movprfx 检查实测 ~6%**（`parse` −6.4% / `fsm` −6.0%），但**不是因为那两个整数比较**——是
  `form_hash_` 的读-改-写依赖 + 一个值跨 leaf 调用存活。典型的「测了才知道」。
- **operand 不该「存」**：存全套要给 entry 加 16–32 字节，得不偿失；该做的是 specialize + inline
  让重抽变成被调度器吸收的廉价直线代码（0 额外空间）。只有少数贵派生（如 `DecodeImmBitMask`
  逻辑位掩码）值得真存。
- **flatten 做彻底（连 NEON/SVE 的 `Simulate_*` 一起）之后可以完全抛弃 `form_hash`**：删掉
  hub 里的 per-instruction 写，并**腾出 entry 里的 32 bit**（entry 因指针对齐仍是 16 字节），
  这 32 bit 正好是上面那些贵派生操作数的家。两条线在这里闭环。

## 自时间拆分

`sample` 的「按栈顶自时间」扁平视图，占比按各自总采样（`parse` ~10238 / `fsm` ~10250）。

| 桶 | 含义 | parse | fsm |
|---|---|---|---|
| **dispatch hub**：`ExecuteInstructionCached` + `RunFrom` + `GabyHookedWritePc` | 每条指令固定开销：cache range 检查、`form_hash_` 写、movprfx 双比较、间接调用、`IncrementPc`/`UpdateBType`/`last_instr` 记账 | **38.6%** | **42.1%** |
| **Visit\* 入口层** | leaf 入口：含 Mask 二次派发 + 部分操作数抽取 | 9.1% | 16.3% |
| **共享 Helper**：`AddSubHelper` / `LoadStoreHelper` / `LogicalHelper` | 操作数重抽 + 一个 Mask switch + 寄存器读写 | 35.4% | 22.0% |
| **操作数派生子程序**：`IsLoad` / `CalcLSDataSize` / `GetImmPCOffsetTarget` / `DecodeImmBitMask` | 纯字段解码 | 6.1% | 5.4% |
| **真语义**：`AddWithCarry` / `ConditionPassed` | 不可约的算子 | 8.7% | 13.6% |
| 未归因（`???` stub） | — | ~2.1% | ~0.6% |

单条最热函数：两个核都是 `ExecuteInstructionCached`（`parse` 3253 / `fsm` 3679 采样），其次
`AddSubHelper`。`fsm` 第三热是 `VisitConditionalBranch`（772），这个 leaf **完全没有二次派发**，
成本全在 `GetConditionBranch` + `GetImmPCOffsetTarget` 操作数抽取，加上 `ConditionPassed` /
`GabyHookedWritePc` 没被内联的调用开销。

## 关键发现

### 1. 「二次派发 + virtual」是小头，flatten 的价值在结构

挖了热 leaf 的源码：

- `AddSubHelper`（`parse` 17.7% / `fsm` 16.2%）里二次派发就**一个** `Mask(AddSubOpMask)` switch，
  剩下全是 `GetSixtyFourBits` / `GetFlagsUpdate` / `GetRn` / `GetRnMode` / `GetRd` / `GetRdMode`
  这些操作数重抽 + 寄存器读写。
- `(this->*pmf)(pc_)` 这个虚调用埋在 hub 的 38–42% 里，但 hub 的钱主要不是它花的——cache 让
  每个 PC 的 leaf 目标稳定，间接分支预测器（Apple 核 ITTAGE 类）预测得很好。**单独 devirtualize
  拿不到多少。**

所以字面「二次派发 + virtual」直接成本约 **5–10%**。真正能拿的是**结构性塌缩**：

- **调用分层**：`ExecuteInstructionCached → Visit* → AddSubHelper → AddWithCarry`，每条指令
  3–4 层非内联调用。`AddSubHelper` 被 Shifted/Extended/Immediate **四个 Visit 共享**，所以
  谁也内联不进去。
- **跨层重复抽取**：`VisitAddSubShifted` 先 `GetSixtyFourBits` 算 `reg_size`，转手 `AddSubHelper`
  又 `GetSixtyFourBits` 算一遍（源码里看得见）。
- 把每个 form specialize 成自己的内联 handler，就同时干掉了：那层调用、那次重复抽取、那个
  Mask switch。

### 2. dispatch hub 大头是固定记账，不是虚调用

`ExecuteInstructionCached` 每条指令固定做：cache range 检查 → BTI gate（已经用 `entry->flags`
bit 0 gate 了，好榜样）→ **movprfx 双 `_h` 比较（没 gate）** → `form_hash_` 写 → 间接调用 →
`last_instr_` / `IncrementPc` / `LogAllWrittenRegisters` / `UpdateBType` 记账。

其中 `form_hash_` 写是**冗余的**——它只为 NEON/SVE 的 `Simulate_*` leaf 服务，标量 Visit* 根本
不读 `form_hash_`（见 §4）。

### 3. movprfx 检查：实测 ~6%，但不是因为整数比较

`"movprfx_z_z"_h` 是 `constexpr uint32_t`（`utils-vixl.h:1524`），所以那两行就是
`form_hash_ == 常量1 || form_hash_ == 常量2`，两个 u32 立即数比较。做了个 ablation
（把 `last_instr_was_movprfx` 强制 `false`，重建，各 3 次，区间不重叠）：

| kernel | baseline（有检查） | ablated（→ false） | 提速 |
|---|---|---|---|
| parse | 7.19 ns/insn (7.17–7.28) | 6.73 (6.62–6.78) | **−6.4%** |
| fsm | 6.79 ns/insn (6.78–6.92) | 6.38 (6.36–6.40) | **−6.0%** |

纯 ALU 的两个 `cmp` 不该值 6%。钱花在二阶效应：

```cpp
bool last_instr_was_movprfx = (form_hash_ == C1) || (form_hash_ == C2);  // 读旧 form_hash_
form_hash_ = entry->form_hash;                                          // 写 form_hash_
(this->*pmf)(pc_);                                                       // leaf 可能读 form_hash_
if (last_instr_was_movprfx) { ... }                                     // 跨整个 leaf 调用的分支
```

1. **`form_hash_` 上的读-改-写依赖**：先读旧值、紧接着写新值、leaf 里又读——这串在 hot loop
   里形成内存序依赖。
2. **`last_instr_was_movprfx` 跨 leaf 调用存活**：末尾那个分支要求这个 bool 在
   `(this->*pmf)(pc_)` 前后保持 live → 多一个 callee-saved 寄存器的 spill/reload。

真实现（gate 到 predecode flag bit，或拿个 member `prev_was_movprfx_` 只在执行 movprfx entry
时置位）拿不到 ablation 的满 6%（`= false` 让编译器把末尾分支也删了），但能吃掉大头。这是个
**实测过的、明确值得做的小赢**。

### 4. operand 怎么存：「存 vs 重抽」是伪二选一

担心「存全套占空间、不存就还得重抽、提速一般」是对的——但有第三条 0 空间的路：

- **全存进 entry**：Rd/Rn/Rm/shift/imm/size 一套要再加 16–32 字节。cache 按 PC 流式访问，
  entry 翻倍 = D-cache footprint 翻倍。tight loop（业务核，L1 装得下）还好，代码一大就亏。
- **specialize + inline**：profile 显示 Helper 桶（`parse` 35% / `fsm` 22%）的大头**不是字段
  解码的算术**（`ubfx Rd` 就一两条），是 §1 说的调用分层 + 跨层重复抽取。specialize 之后字段
  解码还是每次做，但变成几条被调度器吸收掉的内联 `ubfx/and`——**0 额外空间**。
- **只存贵派生**：真正贵到值得存的是逻辑位掩码立即数（`DecodeImmBitMask` 是个真循环）这类。
  但业务核里这种极少（`DecodeImmBitMask` 才 0.3%），所以对业务核「存操作数」几乎不赚。

换句话说，真正该缓存的不是操作数，是**派发决定**（命中哪个专用 handler）——而这就是 dispatch
flatten 本身。两件事是同一件事。

### 5. flatten 彻底之后可以完全抛弃 `form_hash`

`form_hash_` 的读者按是否在执行热路径分类（全核对过）：

| 读者 | 数量 | 执行热路径？ | 挡不挡抛弃 |
|---|---|---|---|
| `switch (form_hash_)` —— 全在 `Simulate_*` / `SimulateSVE*` | 82 | 是（NEON/SVE leaf） | **挡** |
| `form_hash_ ==` —— 多在那些 Simulate_* 体内 + 2 个 movprfx | 42 | 是 | 挡 |
| `CanTakeSVEMovprfx(form_hash_)` | 2 | 是（要 flag-gate 掉） | 顺带 |
| `disasm-aarch64.cc` 的 find | 4 | 否（反汇编） | 不挡 |
| `cpu-features-auditor-aarch64.cc` 的 find/count | 2 | 否（独立 visitor，非 cache track） | 不挡 |
| `VIXL_ASSERT(modes.count(form_hash_))` 等 | ~4 | 否（NDEBUG 编掉） | 不挡 |

**执行热路径上读 `form_hash_` 的，精确地就是 `Simulate_*`（NEON/SVE）那一族。** 所以：

- 只 flatten 标量 Visit*：业务核提速，但 `form_hash_` 的写和字段**得留着**（NEON/SVE entry 仍读它）。
- flatten **两族**（Visit* + Simulate_*，那 82 个 switch）：执行轨没人读 `form_hash_` 了 →
  hub 里的 per-instruction 写变死代码删掉（§2 那块记账的一部分）→ entry 的 `form_hash` 字段
  腾出 **32 bit**。

entry 当前布局 `{uint32_t form_hash; uint32_t flags; const void* leaf;}` = 16 字节
（`predecode_cache.h:94`）。丢掉 `form_hash` 后因指针 8 字节对齐**仍是 16 字节**
（`flags 4 + 复用 4 + leaf 8`）——白得一个 32 bit 槽，entry 一点没胖。这 32 bit 正好是 §4 那些
贵派生操作数的家；设计文档 `predecode_cache.h:103` 早就把 flags 预留 bit 写给
「operand pre-extraction」了。

**范围抉择**：`Simulate_*`（82 个 NEON/SVE switch）是大头机械活，对业务核**提速无感**（冷路径），
它的价值在「解锁 form_hash 移除 + 32 bit + 全工作负载一致」。要不要把它纳入第一刀，是个真实的
排期选择，不在本文档下结论。

## 由数据收敛出的 milestone-1 形状（备忘，非承诺范围）

1. flatten 二次派发 → 专用内联 handler（标量 Visit* 必做；`Simulate_*` 视范围抉择）；
2. 顺带去虚化、塌缩 `Visit*→Helper` 调用层 + 消跨层重复抽取；
3. movprfx flag-gate（实测 ~6%）；若 §5 走「两族全 flatten」，则一并删 `form_hash_` 写；
4. 回收的 32 bit 留给 `DecodeImmBitMask` 这类贵派生 —— entry 恒 16 字节。

**诚实的幅度预期**：若 flatten 按「specialize + inline 整条 leaf 链」做（不是只删 switch），现实可达
总时间 ~25–35%，即业务核 ~1.3–1.5x。残量是不可约语义（`AddWithCarry` / `ConditionPassed`
9–14%）+ hub 的必要最小开销（range 检查 + PC 推进 + 那一次调用）。**这一刀本身到不了 50x**——
它是干净的结构性垫脚石，而且对 `fsm` 这种最差场景相对更划算。

## 复现

```bash
cmake -S . -B build/profile -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DGABY_VM_BUILD_BENCHMARKS=ON -DGABY_VM_BUILD_NATIVE_BASELINE=ON \
  -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer"
cmake --build build/profile --target bench_business

# 后台跑 + 采样
./build/profile/bench/bench_business --kernel parse --mode cache --seconds 30 & \
  BPID=$!; sleep 1; sample $BPID 12 1 -file /tmp/parse_sample.txt -mayDie; kill $BPID
# 扁平自时间：在 sample 输出里看 "Sort by top of stack"
```

movprfx ablation：把 `simulator-aarch64.h:1584-1585` 的 `last_instr_was_movprfx` 计算
临时改成 `false`，重建 `bench_business`，对比改前后的 `ns_per_instruction`（各跑 3 次取中位）。

## 相关文档

- [`gaby-vm-business-bench-2026-06-08.md`](./gaby-vm-business-bench-2026-06-08.md) — 业务核 bench 首份数据与方法学。
- [`gaby-vm-cache-hotpath-profile-2026-05-27.md`](./gaby-vm-cache-hotpath-profile-2026-05-27.md) — smoke / mixed 的 cache 热路径 profile（**锚点**）。
- [`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md) — 预解码/分发缓存权威设计（**锚点**）。
