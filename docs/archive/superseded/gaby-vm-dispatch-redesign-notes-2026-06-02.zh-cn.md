# Gaby-VM Dispatch 重设计想法（粗记）— 2026-06-02

> **它是什么**：一份方向性的粗记。源头是
> [`gaby-vm-cache-hotpath-profile-2026-06-02.md`](./gaby-vm-cache-hotpath-profile-2026-06-02.md)
> 里"Phase 1 设计"那次讨论——讨论着讨论着发现 roadmap 的 Phase 1（收缩当前
> dispatch）和 Phase 2（操作数预解码）其实是在给一个半成品打补丁，真正想要的是
> 换掉执行模型本身。这份就是把那个"换模型"的想法记下来，免得丢。
>
> **它不是什么**：不是定稿设计，不是 OpenSpec change，不是排期。**当前不做**——
> roadmap 的 Phase 1 / Phase 2 都先搁置，近期先做更简单的事（见 §6）。等以后真要
> 动这块，再据此起正式 design。源码引用用 `path:line`，路径相对仓库根。
>
> 同 tier 的现有 doc：[`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md)、
> [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)。

## 1. 核心诊断：现在的 cache 是个半成品

`predecode once → cache → execute` 这条线，今天只缓存了**第一级派发**（form → leaf
函数）。**第二级派发（leaf 里再 decode 选具体操作）和操作数抽取，每次执行都在重做。**

以 smoke 最热的 `VisitLogicalShifted`（`simulator-aarch64.cc:4263`）为例，一条指令
每次执行干三件事：

```
VisitLogicalShifted(instr)                       ← cache 只缓存了「form → 这个函数」
  reg_size = instr->GetSixtyFourBits()           ┐
  shift    = instr->GetShiftDP()                 │  ① 重抽操作数（每次从 instr bit 抠）
  amount   = instr->GetImmDPShift()              │
  op2 = ShiftOperand(.., instr->GetRm(), ..)     ┘
  LogicalHelper(instr, op2):
    op1 = ReadRegister(instr->GetRn())           ← 再抽一个
    switch (instr->Mask(LogicalOpMask & ~NOT))   ┐  ② 二次派发：重新 decode 选
      case AND: result = op1 & op2; ...          ┘     AND/ORR/EOR/ANDS
                                                 ← ③ 真正语义：op1 & op2，就一行
```

两种二次派发形态，本质同一件事——**leaf 拿到的不是「具体要干啥」，还得自己再 decode**：

- **`Visit*` 族**（smoke 热点）：二次派发走 `instr->Mask(...)`。
- **`Simulate_*` 族**（SVE/NEON）：二次派发走 `switch (form_hash_)`。`.cc` 里有
  **~125 处** `form_hash_` 引用，基本都是这种 switch。例：`Simulate_PdT_PgZ_ZnT_ZmT`
  （`simulator-aarch64.cc:2335`）按 `form_hash_` 在 `match`/`nmatch` 之间选。

profile 数据印证：smoke 的 ALU leaf ~44%，描述就是"每次还在从 Instruction 里重抽
Rn/Rm/Rd/shift/imm"。这正是字节码解释器（Lua/JS）**根本没有的税**——它们的字节码
就是 decode 好的。要"不慢于 Lua/JS"（profile §8.2 的真目标），结构性地干掉这层
重复 decode 比抠 dispatch 收益大得多。

## 2. 目标模型：已 decode 的、可穿线的解释器

理想的 decode entry 是**自包含**的：预抽好的 reg/imm + 一个简单 C 函数指针 leaf。

```c
struct DecodedEntry {
  void (*fn)(const DecodedEntry* e, Sim* s);   // 普通 C 函数指针，不是 pmf
  // pre-extracted operands（具体字段随设计定，见 §4①）
  uint8_t  rd, rn, rm;
  uint32_t imm;
  ...
};

// leaf 不再 decode、不读 form_hash，操作数已在 e 里：
void leaf_and_shifted(const DecodedEntry* e, Sim* s) {
  s->x[e->rd] = s->x[e->rn] & shift(s->x[e->rm], e->shift, e->amount);
}
```

把今天每次执行重做的「第二级派发 + 操作数抽取」**一次性压进 predecode**，运行期
`entry->fn(entry, sim)` 就够了。要点：

- **`form_hash_` 最终去掉**（热路径上）。它存在就是为了 leaf 的二次派发；leaf 一旦
  被 predecode 选到具体操作，热路径不再需要它。注意是"最终"——见 §5 的迁移路径，
  它退场在最后。
- **去掉 Simulator 的虚继承**（在热路径上）。`Visit*` 是 `virtual`
  （`simulator-aarch64.h:1756`），今天的 pmf/vtable 间接就来自这里；新 leaf 是自由
  函数 / 静态函数，吃显式 `Sim*` 参数，热路径彻底不走虚分派。（但继承这层皮**建议
  留着**，见 §4③。）
- **天然通向 threaded interpreter**。`fn(entry*, sim*)` 这个签名正是 threaded code
  的形状：leaf 末尾接 `return next->fn(next, sim);`（尾调用）就是 subroutine
  threading——每个 form 各自一个间接跳转点、各自的分支预测历史，正好治 profile §8.1
  说的"单个多态 blr 的 misprediction"这个 dispatch 真天花板。架构天生兼容，不用为它
  返工。

## 3. 护栏：这事能做，但 "no new IR" 的线要划死

AGENTS.md 三条护栏这个改动都蹭到了——"not a new complex IR"、"keep VIXL's leaf
execution code wherever practical"、"do not rewrite simulator leaf semantics"。能调和，
但必须把边界写死：

```
✅ 仍算「decode once」（允许）         ❌ 越界成「新 IR」（禁止）
────────────────────────────         ────────────────────────────
每条 guest 指令 = 一个 entry         跨指令分析 / 基本块拼接
predecode 选好具体 leaf              常量折叠 / 死代码消除
predecode 抽好操作数存进 entry       寄存器分配 / 指令重排
entry 挂 next 指针做 threading       任何打破「1 guest 指令 ↔ 1 entry」的优化
```

**不变量：一条 guest 指令 = 一个 entry = 一次 leaf 调用，语义不变。** 守住它就还是
decode cache，不是 IR。

**和"不许重写 leaf 语义"调和**：specialized leaf 的函数体还是 VIXL 原语义（`result =
op1 & op2`、NEON lane 循环、`ShiftOperand`、饱和/舍入全部原样复用）。**搬的是 leaf
外面那圈 decode 脚手架，不是里面的算术**——变的只是操作数怎么到手（预抽 vs 现抠）、
leaf 怎么被选中（predecode vs 每次执行）。这一点要白纸黑字进将来的 design 当 non-goal。

## 4. 三个绕不开的硬设计点（待定）

**① entry 装不下了——16 字节 flat array 要破。**
现在 entry 是 `{form_hash:4, flags:4, leaf:8}` 正好 16B（`predecode_cache.h:114` 有
`static_assert`）。预抽操作数塞不进。三条路：

- **(a) 加宽自包含**（32/64B，`{fn, operands...}`）：一条指令的全部数据在一个 cache
  line，对 threaded 路径局部性最好；代价是 flat array 密度降。
- **(b) 16B + 侧表索引**（`{fn, side_idx}` → 操作数在另一数组）：保住 16B；代价是多
  一次 indirection，抵消部分收益。
- **(c) 按 leaf 类型 union**：操作数布局随 form 变；密度和自包含兼得，但设计复杂。

倾向 (a)，但这是定 cache 内存形状的决定，待拍。

**② specialized leaf 数量会爆，iOS 还盯着包体积。**
一个 `VisitLogicalShifted` 背后是 `{AND,ORR,EOR,ANDS}×{NOT 变体}×{32/64}×{LSL/LSR/
ASR/ROR}`，全展开几十个；几百个 form 全展开上千个函数。两种现实做法：

- **只在「便于分支的轴」specialize**，其余留作 entry 字段（如每个逻辑操作一个 leaf，
  reg_size/shift 当数据传进去）。
- **template 生成网格**（`template<Op,RegSize,Shift>`，编译器吐叉乘）：省手写，但
  **代码体积换速度**，iOS 上要量。

注意：不必把每个轴都 specialize 到底。每多一个轴 → leaf 数 ×、包体积涨、icache 压力
大。最优点通常是"把最贵的那次重复 decode 干掉就够"，别追求极致 specialize。

**③ 去掉继承——和性能脱钩，排到最后。**
性能收益**全部来自** flat entry + specialized leaf + threading，**不来自删
`: public DecoderVisitor`**。热路径一旦不调 `Visit*`，那个虚继承在热路径上就是死重、
不花钱；predecode 用的是独立的 `FormCaptureVisitor`（`predecode_cache.cc`），也不碰
Simulator vtable。

而且删它有代价：`ExecuteInstruction`（decoder track）是 **shadow_runner 的参考
oracle**——`shadow_runner.cc:111-112` 拿 `fast_.StepOnce()`（cache）对
`ref_.DebugStepOnce()`（decoder）逐条对拍。这是重写几百个 leaf 时**唯一的安全网**。
删继承大概率连 decoder track 一起删，等于拆掉验证工具。

**建议**：leaf 抽成自由/静态函数让热路径去虚，但 **Simulator 继续 is-a
DecoderVisitor**，decoder track 留着当 CI 的参考实现。继承这层皮几乎不花钱，却保住
对拍。等新模型跑稳、覆盖够了，再单独议拆不拆——那时它就是个纯清理的小 change。

## 5. 去风险的迁移路径（真要做时）

别一把梭重写全部。增量迁移：

```
1. 基础设施：新 entry 布局（加宽）+ 新 leaf 签名 void(*)(const Entry*, Sim*)
2. 只给 smoke / 真实负载热点那几个 form 写 specialized leaf
   （logical-shifted、add-sub-shifted/imm、move-wide、load-store-pair …）
3. 其余所有 form 挂 trampoline leaf：它干今天的事（写 form_hash_、调原 pmf）
   → 没迁移的 form 照常工作、零回归
4. predecode 决定每条指令挂 specialized 还是 trampoline
5. 量；shadow_runner 逐条对拍新 leaf
```

这样 `form_hash_` **不是第一天删**——它活在 trampoline 路径上，等最后一个 form 迁完才
退场（§2 说的"最终去掉"就是这个意思）。blast radius 小、可测、每步有 oracle 兜底。
长尾冷 form 可以一直挂 trampoline，永不重写也不影响正确性；按 profile 热度先写最热的
几个就拿到大头收益。如果第一刀下去 smoke 没像预期砍掉那 44% 重抽 + 二次派发，说明
模型假设有问题，趁早发现，没赔上几百个 leaf 的工。

## 6. 当前状态与待定

- **状态**：只记录，不实施。roadmap 的 Phase 1 / Phase 2 **搁置**；近期先做更简单的
  事（具体"简单的"是什么待定——大概率是 profile §8.2 的 Step 0：Lua/JS 真机
  head-to-head 基准，先把"不慢于 Lua/JS"这把尺子立起来，再回头判断这个重设计值不值、
  从哪刀切）。
- **待拍的岔路**（定了才好起正式 design）：
  1. 范围：先切一刀验证模型（§5 那个增量），还是先把完整模型设计成一份大 design。
  2. entry 布局：加宽 (a) / 16B+侧表 (b) / union (c)。
  3. 继承：接受"热路径去虚、留 DecoderVisitor 当 oracle"的折中，还是连 decoder track
     一起拆（要先想清楚拿什么当参考实现）。
  4. threading 时机：第一刀就穿线，还是先 flat entry + specialized leaf、threading 留
     下一刀（倾向后者，一次只验证一个变量）。

## 7. 索引

- 触发这份笔记的 profile + roadmap：
  [`gaby-vm-cache-hotpath-profile-2026-06-02.md`](./gaby-vm-cache-hotpath-profile-2026-06-02.md)
  （§6 roadmap、§8.1 dispatch 深挖、§8.2 目标再校准到 Lua/JS）。
- cache 现有设计：[`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md)
  （§4.2.1 / §4.6 是操作数预解码 / per-form thunk 的原始预留）。
- 相关源码：
  `Sources/gaby_vm/src/aarch64/simulator-aarch64.h:1530-1609`（`ExecuteInstructionCached`）、
  `:1756`（`Visit*` 的 `virtual` 声明宏）、`:1695`（`ResolvePredecodeLeaf`）、
  `:5780`（`FormToVisitorFnMap` typedef）；
  `simulator-aarch64.cc:2306`（decoder track 的 `Visit` 派发，也用这张 map）、
  `:4263`（`VisitLogicalShifted`）、`:2335`（`Simulate_*` 的 `form_hash_` 二次派发样例）；
  `Sources/gaby_vm/include/gaby_vm/predecode_cache.h:94`（`PredecodedEntry`）；
  `Sources/gaby_vm/src/gaby_vm/predecode_cache.cc`（predecode populate pass、BTI flag bit 套路）、
  `shadow_runner.cc:101-112`（cache vs decoder 逐条对拍 oracle）。
