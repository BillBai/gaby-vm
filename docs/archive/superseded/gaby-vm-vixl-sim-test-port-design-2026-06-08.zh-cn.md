# Gaby-VM 移植 VIXL 模拟器测试作 guard rail — 设计稿 — 2026-06-08

> **它是什么**：一份**已和负责人对齐、用来驱动实现计划**的设计稿。目标是把 VIXL
> AArch64 模拟器执行类测试成规模搬进 gaby-vm，在动手做更激进的 dispatch / 操作数预解码
> 性能优化**之前**立一道正确性 guard rail。源头需求：见
> [`gaby-vm-dispatch-redesign-notes-2026-06-02.md`](./gaby-vm-dispatch-redesign-notes-2026-06-02.md)
> 与 [`gaby-vm-cache-hotpath-profile-2026-06-02.md`](./gaby-vm-cache-hotpath-profile-2026-06-02.md)
> ——那两份记录了即将动的热路径，而这道 guard rail 就是给它们兜底的。
>
> **它不是什么**：不是 OpenSpec change（规范性测试要求若要沉淀，后续再并入
> [`../../openspec/specs/aarch64-simulator/spec.md`](../../openspec/specs/aarch64-simulator/spec.md)）。
> 不是排期表。源码引用用 `path:line`，路径相对仓库根；`../vixl` 指参考 VIXL 树。
>
> **状态**：设计已批准（2026-06-08），下一步进 writing-plans 出实现计划。

## 1. 为什么现在做

即将动的优化（dispatch 收缩、操作数预解码、线程化派发）改的是**所有指令共享的执行
热路径**，但必须保持 VIXL leaf 语义一字不差。这类改动最容易悄悄改坏的是**可观测行为**：
寄存器结果、标志位（NZCV）、内存副作用、副作用顺序、trap/退出行为。

今天的回归网（[`../../test/simulator_correctness.cc`](../../test/simulator_correctness.cc)）
只覆盖了 ADD/SUB/MUL、AND/ORR/EOR、基本访存、条件分支、BL/RET 等少数几族，网眼太粗。
一旦优化碰坏某个不常见指令形态，现有测试大概率察觉不到。所以先把 VIXL 那套覆盖面极广的
执行测试搬过来，再动刀。

## 2. 决策记录（已对齐）

| 决策 | 选择 | 理由 |
| --- | --- | --- |
| 覆盖范围 | **广覆盖语义移植** | 搬 `test-assembler-aarch64/-fp/-neon` 三文件里的执行类用例（整数/逻辑/访存/分支/FP/NEON 主体），约 600+ 个 `TEST`。覆盖面广，最能在共享热路径上兜住任意指令形态的回归。 |
| 取字节方式 | **编写期提取工具**（authorship-time：写测试代码时离线跑一次、产物提交进仓库，build/runtime 都不依赖它） | 该工具链接 `../vixl` 真 MacroAssembler+Simulator，把 VIXL 测试体喂进去吐出编码字节 + 期望状态，再嵌进 gaby-vm fixture。Tier-0 完全不进 gaby-vm 的 build/runtime（符合 [`vixl-extraction-map.md`](./vixl-extraction-map.md) 的边界）。 |
| oracle | **差分 + 绝对值都要** | 主力是 cache 轨 vs decoder 轨逐项对拍（decoder 轨=未改动的 VIXL 语义，优化只动 cache 轨，分歧即回归）；再加 VIXL 实测的断言目标作绝对锚，防两轨一起漂。 |

编写期（authorship-time）外部工具不违反 Tier-0 禁令——其产物是「一次性生成、提交进源码」的
数据，build / runtime 都不依赖汇编器。这与现有「llvm-mc 在编写时产生 hex、手抄进源码」的编码
政策是同一性质的延伸，只是把
工具从 llvm-mc 换成 VIXL 自己的汇编器（更忠实）。

## 3. 总体架构：开发期采集 / 发布期回放，两段分离

```
  开发期（需 ../vixl，手动触发，不进 CI/iOS）         发布期（自包含，进 CI/iOS）
 ┌──────────────────────────────┐                 ┌─────────────────────────────┐
 │ tools/vixl_test_extract/     │   生成并提交      │ test/vixl_port/             │
 │  · capture 头（宏重定义）      │  ───────────▶   │  · generated/*.inc (fixture)│
 │  · #include VIXL 真测试 .cc   │   committed      │  · 通用回放 harness          │
 │  · 链接 ../vixl 真 MASM+Sim   │    .inc 头        │  · vixl_port_{int,fp,neon}  │
 │  → 吐 fixture + 过滤报告       │                 │    三个 CTest 可执行文件      │
 └──────────────────────────────┘                 └─────────────────────────────┘
        依赖 ../vixl + Tier-0                          只依赖 gaby_vm 公共 API
```

**核心约束**：生成的 fixture 头**提交进仓库**。平时 `cmake --build && ctest` 完全自包含，
不需要 `../vixl`、不碰汇编器、iOS / CI 构建干净。只有「刷新或新增用例」时，开发者才需要打开
提取工具那条路（`-DGABY_VM_BUILD_VIXL_EXTRACT=ON` + 指定 `../vixl`）。

## 4. 提取引擎（开发期）

### 4.1 宏接缝（已验证可行）

`../vixl/test/aarch64/test-assembler-aarch64.cc:27` 第一条 include 就是它自己的
`test-assembler-aarch64.h`；`SETUP/START/END/RUN` 全在那个头里，且**整段被
`#ifdef VIXL_INCLUDE_SIMULATOR_AARCH64` 包着**（VIXL 本就支持「模拟器模式」选择）。
`TEST(name)` 只展开成注册宏 `TEST_(name)`（`../vixl/test/test-runner.h:145`），
`ASSERT_EQUAL_*` 也只是普通宏（`../vixl/test/aarch64/test-assembler-aarch64.h:317` 起）。

因此可以：定义 `VIXL_INCLUDE_SIMULATOR_AARCH64`；用一个 capture 头**占掉
`test-assembler-aarch64.h` 的 include guard 并提供我方版本的
`TEST/SETUP*/START/END/RUN/ASSERT_EQUAL_*` 宏**；然后 `#include` VIXL 的测试 `.cc`。
每个 `TEST` 就变成一次「用真 MacroAssembler 发射 → 在真 Simulator 上跑 → 采集」的过程。

### 4.2 A1 主引擎 + A3 兜底

- **A1（主）**：宏重定义整文件吞入。~600 条近零人工，VIXL 升级测试时重跑即可。
- **A3（兜底）**：A1 吃不下的个别测试（多次 `RUN()`、特殊 teardown、宏冲突），退化为把那个
  `TEST` 体**手工 copy 进工具**——测试体本就是合法 VIXL 代码，粘进去即可编译运行。

### 4.3 每条采集什么

```
PortedCase {
  name            : 测试名（带源文件前缀，便于回溯 VIXL）
  required_features: 该测试声明 + 实际用到的 CPUFeatures
  code            : 发射出的完整指令字序列 uint32_t[]（含 START 序言 / END 尾声）
  initial_state   : 进入执行点（RunFrom 前）的架构状态快照（X0-30/SP/PC/V0-31/NZCV/FPCR/FPSR）
  asserts         : 每条 ASSERT_EQUAL_* 的目标 = { kind: reg64|reg32|reg128|nzcv|fp,
                                                 reg_id, expected_bits }
}
```

### 4.4 降风险：先跑通再铺量

先在 ~10 条整数用例上把「采集 → 生成 → 回放对拍」端到端跑通，再逐文件铺量。对回放
harness 本身按 TDD 写（先写会失败的回放断言，再实现）。

## 5. Fixture 格式：生成的 C++ 数据，直接嵌入

生成 `constexpr` 数据（指令字 `uint32_t[]`、初始 `RegisterFile`、断言目标数组），**不走运行期
文件 IO、不引 JSON 解析器**——贴合 gaby-vm「predecoded data 就是普通 data」+ 零运行期依赖
的调性。生成产物按指令族切块，提交到 `test/vixl_port/generated/`。
`RegisterFile` 直接复用公共 API 的冻结 POD 布局
（[`../../Sources/gaby_vm/include/gaby_vm/registers.h`](../../Sources/gaby_vm/include/gaby_vm/registers.h)）。

## 6. 回放 harness + oracle 契约

每条 fixture：在两条轨上从同一 `initial_state` 跑同一段 `code`，然后：

- **差分 oracle（主力，覆盖全部用例）**：cache 轨（`RunFrom`）vs decoder 轨（`DebugRunFrom`），
  整个寄存器文件逐项对拍。复用现有 ShadowRunner 思路
  （[`../../Sources/gaby_vm/include/gaby_vm/shadow_runner.h`](../../Sources/gaby_vm/include/gaby_vm/shadow_runner.h)）。
  decoder 轨就是未改动的 VIXL 语义，优化只动 cache 轨——任何分歧即回归。
- **绝对 oracle（锚，防两轨一起漂）**：两条轨都必须满足 `asserts` 里的每条目标值。

### 6.1 关键细节——地址重定位

VIXL 采集时代码落在它自己的 host 地址，gaby-vm 回放落在另一地址：

- **差分 oracle 天然免疫**：两轨跑同地址同字节同初始态，看到的一切（含任何绝对地址）都一致，
  分歧只可能来自实现差异。
- **绝对 oracle 只对 VIXL 真实的 `ASSERT_EQUAL` 目标做**：VIXL 作者断言的几乎都是「算出来的值」
  而非裸指针，本身与地址无关，所以 4.3 采集到的正是这批可移植期望值。极少数断言目标本身就是
  地址的（期望值落在代码/栈地址区间），引擎检测后**跳过该条绝对检查**，由差分 oracle 兜底，
  并在报告里列出被跳过的项。

### 6.2 退出与栈

每条采集的 `code` 含 VIXL 的 START/END 序言尾声，正常以 `Ret` 收尾。gaby-vm 构造时把 LR 置为
null 哨兵，`Ret` 到 null LR 即停（与现有测试一致）。栈用现有
[`../../test/embedding_stack.h`](../../test/embedding_stack.h) 提供的 16 KiB 对齐缓冲。
`initial_state` 的 SP 在回放时重定位到 gaby-vm 这块栈基址。

## 7. 特性门控与过滤（让套件常绿）

引擎在**采集期**就过滤掉 gaby-vm 跑不了的用例：

- 用到 gaby-vm 不支持的 CPUFeatures 的测试。
- 踩到那 39 个 `VisitUnimplemented` 形态（MTE：`LDGM/STGM/LD64B/ST64B`；TME：`TSTART/TCOMMIT/TCANCEL/TTEST`；
  BFloat16；EL1 非特权访存 `LDTR*/STTR*`；`WFET/WFIT`）的测试。
- EL1+ / 系统寄存器副作用 / 异常模型相关（超出 V1 EL0 用户态范围，见 [`../../CLAUDE.md`](../../CLAUDE.md)）。

被丢弃的用例在生成报告里**逐条显式列出**（丢了什么、为什么），不静默截断。发布套件保持全绿。

## 8. 构建接线

- **提取工具**：独立 CMake target，默认**不**构建，藏在 `-DGABY_VM_BUILD_VIXL_EXTRACT=ON`
  + `VIXL_SRC_DIR=../vixl` 后面。只有它链接 Tier-0。
- **发布回放测试**：`vixl_port_integer / vixl_port_fp / vixl_port_neon` 三个可执行文件，消费
  `test/vixl_port/generated/` 的头。需要 ShadowRunner / 直接构造 `Simulator` 时走现有 privileged
  build 模式（`PRIVATE` include `src/` + `VIXL_*` defines，见
  [`../build.md`](../build.md)）。`add_test` 进 CTest。无 Tier-0、不依赖 `../vixl`。
- 三个文件分开，CTest 能按指令族报失败，定位到族。

## 9. 分阶段

- **Phase 0**：搭提取引擎 + fixture 格式 + 通用回放 harness，在 ~10 条整数用例上端到端跑通。
- **Phase 1**：`test-assembler-aarch64.cc`（整数/逻辑/访存/分支/系统中 EL0 可跑部分）铺量。
- **Phase 2**：`test-assembler-fp-aarch64.cc`（FP）。
- **Phase 3**：`test-assembler-neon-aarch64.cc`（NEON）。

每阶段：跑引擎 → 过滤不支持 → 生成 fixture → 套件转绿 → 提交（生成产物 + 报告一起进仓库）。

## 10. 刻意不做（YAGNI）

- 不搬 SVE / SVE2 测试（`test-simulator-sve*-aarch64.cc`）——超出 V1。
- 不搬 `test-simulator-aarch64.cc` 那 14 个 golden-trace 穷举输入矩阵（选了「广覆盖」而非
  「最大化」；将来要补输入矩阵可单列一期）。
- 不引入运行期汇编器、不引入运行期数据文件格式。
- 不改 VIXL leaf 语义；若提取/回放暴露出 gaby-vm 与 VIXL 的真实语义差异，那是单独的 bug，按
  marker 约定（[`../conventions.md`](../conventions.md)）另行处理，不在本期 fixture 里「修期望值」掩盖。

## 11. 风险与开放问题

- **R1 宏吞文件的脆性**：A1 依赖 VIXL 测试宏结构稳定。缓解：先在小切片验证；A3 兜底；把 capture
  头里对 VIXL 宏的假设写成注释 + 断言。
- **R2 采集与回放的初始态一致性**：VIXL START 序言会 push callee-saved、设栈。需确保引擎采集的
  `initial_state` 就是 gaby-vm `RunFrom` 入口看到的同一状态，否则绝对 oracle 会假阳。Phase 0 重点验证此项。
- **R3 量大维护**：600+ fixture 的体积与编译时长。缓解：按族切块、生成产物可重跑覆盖、必要时分多个
  TU。若编译时长成问题，再评估是否引入轻量测试框架（当前不引）。
- **O1**：系统寄存器（`MRS/MSR` 中 EL0 可读那部分，如 `CTR_EL0/DCZID_EL0`）要不要纳入？倾向纳入
  可跑的那部分，由特性门控/采集期实测决定，计划阶段细化。
- **O2**：是否在发布套件里额外保留「少量手写、带可读注释的代表用例」作为人类可读的冒烟层，与机器
  生成的大批量 fixture 并存？倾向保留现有 `simulator_correctness.cc` 不动，机器生成的另起目录。

## 12. 验收标准

1. 开发期：`-DGABY_VM_BUILD_VIXL_EXTRACT=ON` 能基于 `../vixl` 生成 fixture，并产出一份「纳入/丢弃」
   报告。
2. 发布期：默认构建（无 `../vixl`、无该 option）下，`vixl_port_{integer,fp,neon}` 三个 CTest 全绿。
3. 每条用例同时通过差分 oracle（cache==decoder 全寄存器文件）与绝对 oracle（满足 VIXL 采集的断言目标，
   地址相关项除外且已记录）。
4. 故意在 cache 轨注入一处缺陷（类似 `shadow_runner_test.cc` 的注入），套件应能红——证明 guard rail
   真的会叫。
5. Phase 1–3 完成后，覆盖的指令族明显宽于现有 `simulator_correctness.cc`，且有可量化的纳入/丢弃清单。
