# Cache Hot-Path Profile — 2026-05

> 解释器 cache 路径在 mixed workload 上的一次 sampling profile。回答的问题：
> 「现在 cache 路径每条指令大概 57 ns，这 57 ns 都花到哪儿去了？」
>
> 这份记录是 `predecode-cache-hotpath-speedup` 落地之后（commit `ca7b580`）
> 的一次定向测量。它直接修正了我们之前对 cache 热路径成本结构的猜测——
> 详见 §4「跟之前的猜测对比」。
>
> 文件名带年月：将来新一轮 profile 会作为新文件落进来（不就地编辑）。
> 这一份当作那个时间点的快照。

## TL;DR

mixed cache 路径的 57 ns/insn 里，**~56% 花在 NEON 格式查询的小函数上**
（`LaneSizeInBitsFromFormat` / `IsSVEFormat` / `LaneCountFromFormat` /
`LogicVRegister::ClearForWrite`），**dispatch overhead 其实只占 ~5%**。

| 类别 | 样本占比 | 备注 |
|---|---:|---|
| NEON 格式 helpers + ClearForWrite | **~56%** | VIXL 把 NEON 寄存器抽象成 `LogicVRegister<VectorFormat>` 的固定成本 |
| `memset` / `memmove`（基本是 ClearForWrite 调的） | ~7% | NEON 寄存器写前清零 |
| `ExecuteInstructionCached`（dispatch 本体） | ~5% | 跟 `predecode-cache-hotpath-speedup` 之后 mixed 性能没动严丝合缝 |
| 各 `Visit*` / `*Helper` leaf 本体 | ~32% | load/store helper、AddSub helper、具体 NEON op leaves |

**结论**：要把 mixed 从 ~300× native 往下推，主要矛盾不在 dispatch，
也不在 leaf 簿记，而在「VIXL 的 NEON 抽象在每条 NEON 指令上反复调一堆
externally-linkable 的 helper 函数」。这些函数本质是 pure function of
`VectorFormat`（6-bit 枚举），把它们 inline 进 header 是潜在最大的杠杆。

## 1. 测量设置

- 机器：Apple Silicon（`Darwin 25.5.0`，arm64），未做 pinning、未关其他进程。
- Build：在 `build/profile/` 单独配的目录，flags：
  ```
  -O3 -g -fno-omit-frame-pointer -DNDEBUG
  ```
  跟 `dev-release` 的差别只是加了 `-g -fno-omit-frame-pointer`，
  让 `sample` 能拿到符号 + 完整调用栈。`dev-release` 本身不动。
- Workload：`bench_baseline --engine cache --seconds 15`
  （VIXL 生成器，seed=42，~65k 静态 word，~64.6k dyn insn/iter）。
- 工具：macOS 自带 `sample <pid> 10 -mayDie -file /tmp/...`，1ms 间隔。
  10 秒采到 **5573** 个样本。
- 输出：`/tmp/mixed_cache_profile.txt`（call tree + sort-by-top-of-stack）。
  这一份记录的是 "Sort by top of stack" 段的扁平计数。

### 复现

```bash
cmake -S . -B build/profile -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGABY_VM_BUILD_BENCHMARKS=ON \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -fno-omit-frame-pointer -DNDEBUG"
cmake --build build/profile --target bench_baseline

./build/profile/bench/bench_baseline --engine cache --seconds 15 &
sample $(pgrep -f bench_baseline | head -1) 10 -mayDie -file /tmp/profile.txt
```

## 2. 热点数据（top-of-stack flat counts）

`sample` 的 "Sort by top of stack" 段，只列 ≥10 样本的项；imported VIXL
helper 用粗体（这一类是接下来最值得动的地方）。

| 样本 | 函数 |
|---:|---|
| **1380** | `vixl::aarch64::LaneSizeInBitsFromFormat(VectorFormat)` |
|  **930** | `vixl::aarch64::IsSVEFormat(VectorFormat)` |
|  **795** | `vixl::aarch64::LogicVRegister::ClearForWrite(VectorFormat)` |
|   309 | `vixl::aarch64::Simulator::ExecuteInstructionCached()` |
|   208 | `_platform_memset` (libsystem) |
|   199 | `_platform_memmove` (libsystem) |
|   129 | `Simulator::LoadStorePairHelper` |
|   117 | `Simulator::LoadStoreHelper` |
|    64 | `Memory::Write<uint8_t, uint64_t>` |
|    56 | `Simulator::FPPairedAcrossHelper<SimFloat16>` |
|    52 | `_xzm_free` (libsystem malloc) |
|    48 | `Simulator::AddSubHelper` |
|    **40** | `vixl::aarch64::LaneCountFromFormat(VectorFormat)` |
|    40 | `Simulator::SimulateFPRoundInt` |
|    38 | `Simulator::LogicalHelper` |
|    35 | `Simulator::VisitMoveWideImmediate` |
|    34 | `Instruction::GetImmPCOffsetTarget` |
|    33 | `gaby_vm::Simulator::RunFrom`（kEndOfSim 检查 + 循环） |
|    33 | `Simulator::VisitLogicalShifted` |
|    32 | `Simulator::MemWrite<uint64_t>` |
|    32 | `Simulator::VisitLoadStoreUnsignedOffset` |
|    30 | `_xzm_xzone_malloc` (libsystem malloc) |
|    30 | `Simulator::st4` |
|    28 | `Simulator::VisitBitfield` |
|    26 | `Simulator::add`（NEON int add） |
|    25 | `Simulator::VisitLoadStorePairOffset` |
|    24 | `Simulator::VisitConditionalSelect` |
|    20 | `Instruction::IsLoad` |
|    20 | `Simulator::VisitNEON3Same` |
|    20 | `Simulator::sshl` |
|    19 | `Simulator::ConditionalCompareHelper` |
|    19 | `Simulator::VisitUnconditionalBranch` |
|    18 | `vixl::aarch64::LaneSizeInBytesFromFormat(VectorFormat)` |
|    18 | `Simulator::VisitPCRelAddressing` |
|    17 | `Instruction::IsStore` |
|    17 | `Simulator::AddWithCarry` |
|    17 | `Simulator::VisitLoadStorePairPostIndex` |
|    17 | `Simulator::and_` (NEON) |
|    16 | `vixl::Float16Classify` |
|    16 | `Simulator::VisitConditionalBranch` |
|    15 | `vixl::Float16ToRawbits` |
|    15 | `Simulator::VisitDataProcessing1Source` |
|    14 | `Simulator::VisitUnconditionalBranchToRegister` |
|    14 | `Simulator::sxtl` (NEON) |
|    12 | `vixl::RawbitsToFloat16` |
|    12 | `Simulator::mul` (NEON) |
|    11 | `vixl::aarch64::CalcLSDataSize(LoadStoreOp)` |
|    11 | `Simulator::VisitFPDataProcessing2Source` |
|    11 | `Simulator::uzp2` (NEON) |
|    10 | `Simulator::ConditionPassed` |

malloc / free 的 ~80 样本来自 `LogicVRegister`/`SimVRegister` 在 leaf
里临时构造时分配的 heap buffer——属于 NEON 抽象的同一笔账。

## 3. 怎么看这张表

`LaneSizeInBitsFromFormat` / `IsSVEFormat` / `LaneCountFromFormat` /
`LaneSizeInBytesFromFormat` 全是 `instructions-aarch64.cc` 里的 ordinary
function definition——一个 switch on `VectorFormat`，返回 8/16/32/64 或
true/false。定义在 `.cc` 里就意味着是 externally-linkable 函数，每次都是
**真函数 call + switch**，没有内联折叠的机会。

每个 NEON leaf（比如 `Simulator::add` for NEON int add）会反复用这些
函数：算 lane count、算 lane bits、判 SVE vs ASIMD、给 `ClearForWrite`
传 format……一条 NEON 指令在 leaf 里调它们 5-10 次很常见。这些 cost
在 mixed 这种 NEON 密集 workload 上是直接叠出来的。

`ClearForWrite` 本身就把 `VectorFormat` 传进去问 lane size，再 memset
对应字节。`_platform_memset` 那 208 样本里大头来自它。

`ExecuteInstructionCached` 自己只占 5.5%——其中已经包含了 dispatch
的全部代价（cache lookup、form_hash 写、pmf indirect call、BType gate、
last_instr 更新、IncrementPc、UpdateBType）。这跟 `predecode-cache-
hotpath-speedup` 测出来 mixed ns/insn 没动（57 vs 57）对得上：dispatch
overhead 在 mixed 上的总盘子太小，省下来看不出来。

## 4. 跟之前的猜测对比

落地 `predecode-cache-hotpath-speedup` 时，我们对 200 cycles/insn
（mixed cache）的拆解大致是：

| 阶段（猜测） | 估计占比 |
|---|---:|
| cache lookup + dispatch | ~10% |
| `Mask` / `Extract` 拆操作数 | ~15% |
| `SimRegister<T>` 抽象 | ~30% |
| NZCV / 内存访问 sandboxing | ~20% |
| `LogAllWrittenRegisters` + `UpdateBType` + 簿记 | ~20% |
| form_hash switch | ~5% |

实测之后**绝大部分都错了**：

- dispatch 占比猜对了（~10% vs 实测 ~5%）。
- `Mask` / `Extract` 在 profile 里几乎看不到——编译器内联折叠掉了。
- "SimRegister 抽象" 那 30% 其实更具体——是 **NEON 这条线特有的
  `LogicVRegister<VectorFormat>` 周边那一坨小 helper**，不是通用 GP
  寄存器抽象。GP 寄存器路径（`AddSubHelper`、`LogicalHelper` 这些）
  加起来只占 ~5%。
- `LogAllWrittenRegisters` / `UpdateBType` 在 profile 里看不到——
  应该被编译器内联到 `ExecuteInstructionCached` 里、并且因为 `trace_`
  这种 flag 是常量 false 被部分死代码消除了。**砍它们的预期收益是 0**。
- NZCV 处理也基本看不到——这部分整体很小。

也就是说，先前给出的「kill log overhead / pre-extract operands /
write fast leaves」这套优化路线**第一条作废、第二条作用有限、第三条
方向对但需要重新挑 hot form**。

## 5. 对优化方向的含义

按现在的 profile 排，下一步 OpenSpec change 的取舍：

### 5.1 高 ROI：NEON 格式 helper 内联

把 `LaneSizeInBitsFromFormat` / `LaneCountFromFormat` / `IsSVEFormat`
/ `LaneSizeInBytesFromFormat` 从 `instructions-aarch64.cc` 挪进
`instructions-aarch64.h`，标 `constexpr` + `inline`。它们本质是 6-bit
枚举上的 lookup，constexpr 之后编译器能在 leaf 里折成立即数（lane
count = 8 直接进 immediate，不再 call function）。

预期：mixed cache ns/insn 从 ~57 ns 砍到 ~30-40 ns（粗估上限是把这
56% NEON-helper cost 砍掉一大半）。VIXL boundary 影响：需要 marker
block，但 inline 之后语义不变，ShadowRunner 能兜底。

### 5.2 中 ROI：`LogicVRegister::ClearForWrite` 跳过

profile 里 ClearForWrite + 它调的 memset/memmove 加起来 ~21%。NEON
destination 在大多数 leaf 里是 write-full-vector，前置清零是浪费。
但语义上能不能跳过得逐 op 看——某些 partial-write 的 op（比如
`ins`、`zip1` 这种）依赖 ClearForWrite 把没写到的 lane 清零。

需要先列清楚哪些 op 是 full-write，再 gate ClearForWrite。

### 5.3 低 ROI（之前以为高 ROI）

- 砍 `LogAllWrittenRegisters` / `UpdateBType`：profile 里看不到这些
  函数本体，**作废**。
- pre-extract `Mask` / `Extract` 操作数到 PredecodedEntry：profile 里
  `Mask` / `Extract` 也看不到（已被内联），**作废**。
- fast leaves for hot forms：方向还对，但需要先看 smoke profile 才
  能确定哪些 form 是真的 hot。

### 5.4 仍然待测

**smoke workload 的 profile 还没采**。smoke 是 branch-free 整数 ALU，
没有 NEON。在 smoke 上分布会完全不同——dispatch overhead 占比预期
会高得多（因为没了 NEON helper 这个大头），是验证「dispatch 改动
能不能压 smoke ns/insn」的关键。下次再做一次 smoke profile，跟这
份并列。

## 6. 对 30× native 目标的判断

mixed 现在 57 ns/insn ≈ ~200 cycles/insn ≈ ~300× native。

- 仅靠 5.1（NEON helper 内联）能进到 ~30-40 ns/insn ≈ ~150-200× native。
- 加上 5.2（ClearForWrite gating）和 5.3 里 fast leaves，乐观估计能
  压到 ~15-25 ns/insn ≈ ~80-120× native。
- 想再往 30× 走，剩下的预算就要在「VIXL leaf 本体的语义抽象」上动
  刀了——不是简单优化，是结构性重写。或者上 JIT，但项目明确说
  iOS embedding 没 JIT 这条路。

所以**纯解释器路线下，把 mixed 推到 ~80-100× native 是现实的中期
目标，30× 在不上 JIT 的前提下不可达**。文档把这条认知留下来。

## 7. 数据/方法的局限

- 单 run，无重复，无中位/方差。5573 样本对 top 函数是稳的，但
  尾部（≤20 样本）噪声大。
- workload 是 VIXL 生成器随机出来的 mixed，**不代表任何真实负载**。
  真实负载的 NEON 比例可能远低于这个，那 §5.1 的杠杆收益就会缩水。
  做完 smoke profile 之后还应该跑一份「typical embedder workload」
  的 profile——比如 zlib 解压、JSON parse 这种——才能定真正的
  NEON cost 占比。
- `_platform_memset` / `_platform_memmove` 没拆调用源。可以通过
  `sample -f /tmp/...` 看完整 call tree 拿到，但这一份只录扁平计数。

## 8. 索引

- 上一份 throughput 快照：[`baseline-benchmark-results-2026-05.md`](./baseline-benchmark-results-2026-05.md)
- cache 路径设计：[`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md)
- bench harness 说明：`bench/README.md`
- 落地 cache 路径优化的 commit：`ca7b580`（`perf(cache): drop std::function indirection ...`）
