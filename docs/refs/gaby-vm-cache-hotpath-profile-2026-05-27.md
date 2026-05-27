# Cache Hot-Path Profile — 2026-05-27

> Cache 路径的第二次定向 sampling profile，跟前一份
> [`gaby-vm-cache-hotpath-profile-2026-05.md`](./gaby-vm-cache-hotpath-profile-2026-05.md)
> 同方法学（macOS `sample`，1 ms 间隔，`build/profile/` flags：
> `-O3 -g -fno-omit-frame-pointer -DNDEBUG`），代码状态相同（commit
> `16f27a9`，等同 `ca7b580` 的产物，只有 docs 增量）。
>
> 这一份相对上次新增/修正的内容：
>
> 1. **新增 smoke 的 profile**——填上前一份 §5.4 留下的「smoke 还没采」
>    那个 to-do。smoke 跟 mixed 的热点分布**几乎正交**，这次有数据可以
>    直接比。
> 2. **修正 sampling 开销的估计**——这次实测 profile build (带
>    `-g -fno-omit-frame-pointer` + 每 1 ms sample) 下的 ns/insn
>    跟 dev-release 的差异：mixed 是 40.69 vs 39.55（**+3%**），
>    smoke 是 6.53 vs 6.49（**<1%**）。前一份在 cache 路径的
>    baseline 文档里把 sampling 干扰估到 ~30%，那个估计是错的，
>    应当按这一份的 ~3% 写。前份 profile 里 mixed cache "~57 ns/insn"
>    那个数字大概率是当时机器状态/负载偏高，**不能用作 cache 路径
>    cost 的参考**——dev-release 下的稳态 39.55 ns/insn 才是。
> 3. **新增 bench harness overhead 这一项**——在 smoke 上能看到，约
>    占 5%。
>
> 文件名带年月日是为了跟前一份不冲突；将来再做 profile 跟着这个
> 格式继续累积。

## TL;DR

- **mixed cache（39.55 ns/insn）**：~68% 的时间是 NEON 抽象代价
  （format helpers + ClearForWrite + memset/memmove + malloc/free），
  跟前一份 profile 的结论一致。dispatch 自身（`ExecuteInstructionCached`
  +`gaby_vm::Simulator::RunFrom`）只占 6.4%。
- **smoke cache（6.49 ns/insn，纯 ALU 无 NEON）**：分布完全不同。
  **dispatch 占 37%**，**leaves 占 42%**，剩下是 Visit\* 入口和 bench
  harness 自己的 timer 开销。这是 cache 路径的"非 NEON 极限态"——
  没有 NEON 抽象代价之后，瓶颈直接暴露成 dispatch + leaves。
- **关键含义**：两个 workload 的优化路径不重叠。砍 NEON 抽象提速 mixed
  对 smoke 几乎没影响；砍 dispatch 循环提速 smoke 对 mixed 只能拿到
  ~6.4% 的天花板。任何统一的优化方案都要按 workload 拆开来排期。

| 类别 | mixed 占比 | smoke 占比 | 备注 |
|---|---:|---:|---|
| NEON 格式 helpers + ClearForWrite + memset/memmove | **~65.7%** | 0% | mixed 第一优先；smoke 不涉及 |
| 临时 LogicVRegister/SimVRegister 的 heap 分配 | ~2.5% | 0% | mixed 唯一 |
| dispatch (`ExecuteInstructionCached` + `RunFrom`) | ~6.4% | **~37.4%** | smoke 第一优先 |
| Visit\* 入口 | ~5% | ~10.9% | 两边都有，smoke 占比高 |
| leaf helpers (AddSub/Logical/LdStPair/...) | ~20% | ~42.3% | smoke 第二优先 |
| bench harness timer + LR reset | <0.1% | ~5.4% | smoke 的 32-insn/iter 摊不动 |

## 1. 测量设置

跟前一份完全相同：

```bash
cmake -S . -B build/profile -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGABY_VM_BUILD_BENCHMARKS=ON \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -fno-omit-frame-pointer -DNDEBUG"
cmake --build build/profile --target bench_baseline bench_smoke

# mixed
./build/profile/bench/bench_baseline --engine cache --seconds 15 &
sample $(pgrep -f bench_baseline | head -1) 10 -mayDie \
    -file /tmp/mixed_cache_profile_2026_05_27.txt

# smoke
./build/profile/bench/bench_smoke --engine cache --seconds 10 &
sample $(pgrep -f bench_smoke | head -1) 8 -mayDie \
    -file /tmp/smoke_cache_profile_2026_05_27.txt
```

- 机器：Apple M4 Pro，macOS 26.5，未做 pinning。
- 样本量：mixed **8566**，smoke **6880**（main thread total，from sample
  的 call graph 头部）。
- 实测 throughput：
  - mixed profile build：40.69 ns/insn（dev-release：39.55，差 ~3%）
  - smoke profile build：6.53 ns/insn（dev-release：6.49，差 <1%）
- 输出：`/tmp/mixed_cache_profile_2026_05_27.txt`、
  `/tmp/smoke_cache_profile_2026_05_27.txt`，两份都包含 call graph
  和 sort-by-top-of-stack 两段。

## 2. Mixed top-of-stack flat counts

只列 ≥10 样本的项。粗体是 imported VIXL 里值得动的 helper。

| 样本 | % | 函数 |
|---:|---:|---|
| **2253** | **26.3%** | `vixl::aarch64::LaneSizeInBitsFromFormat(VectorFormat)` |
| **1517** | **17.7%** | `vixl::aarch64::IsSVEFormat(VectorFormat)` |
| **1036** | **12.1%** | `vixl::aarch64::LogicVRegister::ClearForWrite(VectorFormat)` |
|   496 |  5.8% | `vixl::aarch64::Simulator::ExecuteInstructionCached()` |
|   343 |  4.0% | `_platform_memset` (libsystem) |
|   300 |  3.5% | `_platform_memmove` (libsystem) |
|   177 |  2.1% | `Simulator::LoadStoreHelper` |
|   166 |  1.9% | `Simulator::LoadStorePairHelper` |
| **118** |  1.4% | `vixl::aarch64::LaneCountFromFormat(VectorFormat)` |
|    83 |  1.0% | `_xzm_free` (libsystem malloc) |
|    79 |  0.9% | `Memory::Write<uint8_t, uint64_t>` |
|    71 |  0.8% | `Simulator::AddSubHelper` |
|    67 |  0.8% | `Simulator::SimulateFPRoundInt` |
|    67 |  0.8% | `Simulator::VisitMoveWideImmediate` |
|    63 |  0.7% | `Simulator::VisitLogicalShifted` |
|    62 |  0.7% | `Simulator::MemWrite<uint64_t>` |
|    59 |  0.7% | `Simulator::LogicalHelper` |
|    57 |  0.7% | `vixl::RawbitsToFloat16(uint16_t)` |
|    57 |  0.7% | `Instruction::GetImmPCOffsetTarget` |
|    51 |  0.6% | `gaby_vm::Simulator::RunFrom(uintptr_t)` |
|    49 |  0.6% | `Instruction::IsLoad` |
|    47 |  0.5% | `vixl::aarch64::CalcLSPairDataSize(LoadStorePairOp)` |
|    46 |  0.5% | `Simulator::VisitUnconditionalBranch` |
|    45 |  0.5% | `vixl::Float16ToRawbits` |
|    44 |  0.5% | `Simulator::VisitLoadStoreUnsignedOffset` |
|    42 |  0.5% | `<deduplicated_symbol>` (libsystem_malloc) |
|    39 |  0.5% | `_xzm_xzone_malloc` (libsystem malloc) |
|    37 |  0.4% | `Simulator::FPPairedAcrossHelper<SimFloat16>` |
|    32 |  0.4% | `vixl::aarch64::CalcLSDataSize(LoadStoreOp)` |
|    32 |  0.4% | `Simulator::VisitBitfield` |
|    32 |  0.4% | `Simulator::VisitLoadStorePairOffset` |
|    29 |  0.3% | `Simulator::VisitNEON3Same` |
|    26 |  0.3% | `Simulator::AddWithCarry` |
|    26 |  0.3% | `Simulator::ConditionPassed` |
|    25 |  0.3% | `vixl::Float16Classify` |
|    24 |  0.3% | `vixl::aarch64::RegisterSizeInBytesFromFormat(VectorFormat)` |
|    24 |  0.3% | `Simulator::VisitDataProcessing1Source` |
|    24 |  0.3% | `Simulator::sshl` (NEON) |
|    22 |  0.3% | `Simulator::VisitFPDataProcessing2Source` |
|    22 |  0.3% | `Simulator::VisitLoadStorePairPostIndex` |
|    21 |  0.2% | `Instruction::IsStore` |
|    17 |  0.2% | `Simulator::add` (NEON int) |
|    16 |  0.2% | `Simulator::VisitUnconditionalBranchToRegister` |
|    15 |  0.2% | `Simulator::VisitConditionalSelect` |
|    14 |  0.2% | `Simulator::NEONLoadStoreMultiStructHelper` |
|    14 |  0.2% | `Simulator::and_` (NEON) |
|    14 |  0.2% | `vixl::aarch64::TryMemoryAccess` |
|    13 |  0.2% | `vixl::aarch64::LaneSizeInBytesFromFormat(VectorFormat)` |
|    13 |  0.2% | `Simulator::VisitConditionalBranch` |
|    13 |  0.2% | `Simulator::VisitSystem` |
|    13 |  0.2% | `Simulator::mul` (NEON) |

### 桶 (mixed)

| 桶 | 样本 | % |
|---|---:|---:|
| NEON format helpers (LaneSizeInBits/Bytes/Count, IsSVEFormat, RegisterSizeInBytesFromFormat) | 3925 | **45.8%** |
| `LogicVRegister::ClearForWrite` 本体 | 1036 | 12.1% |
| `memset` / `memmove` / `__bzero`（基本是 ClearForWrite 调的） | 667 | 7.8% |
| Heap alloc / free（LogicVRegister 临时 buffer） | ~218 | ~2.5% |
| **NEON 抽象总开销（上面四项合计）** | **5846** | **~68.2%** |
| `ExecuteInstructionCached`（dispatch 本体） | 496 | 5.8% |
| `gaby_vm::Simulator::RunFrom`（外循环） | 51 | 0.6% |
| Visit\* 入口（VisitMoveWide / VisitLogicalShifted / ...） | ~430 | ~5.0% |
| leaf 本体（AddSub/Logical/LdSt/FP/NEON-int/ConditionalSelect/...） | ~1700 | ~19.9% |
| 其它（unsymbolized stubs、Float16 helpers 等） | ~120 | ~1.4% |

`memset`/`memmove` 那 667 样本绝大部分是 `LogicVRegister::ClearForWrite`
里 `memset(0, ..., lane_size_in_bytes * lane_count)` 调出来的。从 ClearForWrite
的视角看实际成本是它的本体 1036 + 它直接造成的 memset/memmove 667 =
1703 / 8566 = **19.9%**——也就是说仅 ClearForWrite 这一个动作就吃掉 mixed
的约 1/5。

### 跟前一份 profile 的差异（mixed）

前一份的占比放过来直接对：

| 类别 | 前一份 ~占比 | 这一次 占比 |
|---|---:|---:|
| NEON 格式 helpers + ClearForWrite | ~56% | ~58% |
| memset/memmove | ~7% | ~7.8% |
| `ExecuteInstructionCached` | ~5% | ~5.8% |
| 各 Visit\* / \*Helper leaf 本体 | ~32% | ~20% |

差异主要在「leaf 本体」桶——这次拆得更细之后，会发现前一份的 32%
里有一部分其实是 NEON 抽象（ClearForWrite 在内）的 "尾巴"，这次归
进了 NEON 抽象桶。**两次 profile 的结论方向完全一致**，定量略偏。

## 3. Smoke top-of-stack flat counts

| 样本 | % | 函数 |
|---:|---:|---|
| **2275** | **33.1%** | `vixl::aarch64::Simulator::ExecuteInstructionCached()` |
| **1243** | **18.1%** | `vixl::aarch64::Simulator::LogicalHelper` |
| **1207** | **17.5%** | `vixl::aarch64::Simulator::AddSubHelper` |
|   334 |  4.9% | `Simulator::VisitLogicalShifted` |
|   305 |  4.4% | `Simulator::VisitLoadStorePairPostIndex` |
|   298 |  4.3% | `gaby_vm::Simulator::RunFrom(uintptr_t)` |
|   276 |  4.0% | `Simulator::LoadStorePairHelper` |
| _268_ | _3.9%_ | `mach_continuous_time` (libsystem)  ← bench harness timer |
|   182 |  2.6% | `Simulator::AddWithCarry` |
|   103 |  1.5% | `Simulator::VisitUnconditionalBranchToRegister` |
|    46 |  0.7% | `Instruction::IsLoad` |
|    41 |  0.6% | `vixl::aarch64::CalcLSPairDataSize(LoadStorePairOp)` |
| _36_ | _0.5%_ | `clock_gettime_nsec_np` (libsystem)  ← bench harness |
| _25_ | _0.4%_ | `mach_timebase_info` (libsystem)  ← bench harness |
| _23_ | _0.3%_ | `clock_gettime` (libsystem)  ← bench harness |
| _18_ | _0.3%_ | `gaby_vm::Simulator::Write(GpRegister, uint64_t)`  ← `WriteLr(kEndOfSim)` per-iter |
|    13 |  0.2% | `Simulator::SetTraceParameters` |
|     8 |  0.1% | `Simulator::VisitAddSubImmediate` |

斜体的是 bench harness 自己的 timer 开销和 per-iteration `WriteLr` 维护
（不是 simulator 在执行 smoke workload 时的开销）。

### 桶 (smoke)

| 桶 | 样本 | % |
|---|---:|---:|
| **dispatch**（`ExecuteInstructionCached` + `gaby_vm::Simulator::RunFrom`） | 2573 | **37.4%** |
| **leaf helpers**（AddSub + Logical + LoadStorePair + AddWithCarry + IsLoad + CalcLSPairDataSize） | 2995 | **43.5%** |
| Visit\* 入口（Logical + LdpPost + BR + AddSubImm + ...） | ~750 | ~10.9% |
| **bench harness overhead**（mach_continuous_time + clock_gettime\* + WriteLr） | 370 | **~5.4%** |
| 其它（unsymbolized stubs、SetTraceParameters 之类 cold path） | ~190 | ~2.8% |

### 几个值得记下来的观察 (smoke)

1. **`ExecuteInstructionCached` 一个函数 33%**。这是 cache 路径每条
   指令都要跑的 prelude/postlude：
   - `pc_modified_ = false`
   - cur_range_ 命中检查（绝大多数情况下命中本范围）
   - 计算 entry 指针
   - BType flag 检查（mostly 没命中）
   - movprfx 状态检查（smoke 上从不命中）
   - 加载 pmf、`(this->*pmf)(pc_)` 间接 call
   - `last_instr_ = ReadPc(); IncrementPc(); LogAllWrittenRegisters();
     UpdateBType()`
   
   `simulator-aarch64.h:1496-1575`。所有这些工作 smoke 上每条指令要
   做一遍，是 dispatch 路径的"刚性下限"。
2. **`AddSubHelper` + `LogicalHelper` 加起来 35.6%**。smoke workload 是
   28 条 ALU body + 2 条 LDP/STP，所以 ALU helpers 占大头是设计预期的。
   值得注意的是这两个 helper 内部还在每次调用时从 `Instruction` 里
   `Extract` Rn/Rm/Rd/shift/imm——这一步如果搬到 predecode 阶段，
   leaf 里 5-10 条 bitfield 操作可以省掉。
3. **`mach_continuous_time` 3.9%**。bench harness 在 timed loop 的
   每次 `RunFrom` 之间会调一次 `steady_clock::now()`，smoke 上单次
   RunFrom 只跑 32 条指令，所以 timer 调用频率（~4.78 M/sec）跟
   指令吞吐量在同一个量级。这 3.9% 是 harness 测量自身的成本，**不
   是 simulator 的成本**。同理 `WriteLr(kEndOfSimAddress)` 那 18
   样本（0.3%）也是 harness 的。
4. **NEON 相关 helper 全部 0 样本**。smoke 是手写无 NEON 工作负载，
   验证了"NEON 抽象代价"那个桶在 smoke 上完全不出现。这给优化 mixed
   时的对照组提供了干净的 baseline——若把 NEON helper 砍掉，mixed
   的"非 NEON 部分"应该跟 smoke 同档。

## 4. Mixed vs smoke：瓶颈结构

把两边的桶占比并列：

```
                        mixed%    smoke%
NEON abstraction         68%        0%     ← mixed-only
dispatch overhead         6%       37%     ← smoke-dominant
leaf helpers             20%       43%     ← shared
Visit* dispatch entries   5%       11%     ← shared
malloc/free               2%        0%     ← mixed-only
harness timer            <1%        5%     ← measurement artifact
```

读法：

- **mixed 瓶颈在 NEON 抽象**。如果把 NEON helpers 全部 inline 掉、
  ClearForWrite 跳过 full-vector write、heap 临时 buffer 改栈/池，
  mixed 的 68% 这一块大约能砍掉 50-60%——把 mixed cache 从 39.5 ns/insn
  压到 ~20 ns/insn 量级。
- **smoke 瓶颈在 dispatch**。NEON 这一刀对 smoke 完全没用。要在 smoke
  上提速只能动 dispatch overhead（ExecuteInstructionCached 圈 + leaf
  prelude/postlude）。这条 mixed 上也有，但只能拿到 ~6% 的天花板，
  收益跟 NEON 优化没法比。
- **leaf helpers 是两边的共同 ~20-43%**。优化 leaf helpers（预解码
  操作数、fast-form leaves、跳过用不上的 flag 更新）两边都受益，但
  smoke 受益更多——因为 mixed 的 leaf 部分大头本来就被 NEON helpers
  覆盖了。

## 5. 跟前一份 profile 在「优化方向」上的差异

前一份 §5 把方向排成「5.1 NEON helper 内联（高 ROI） → 5.2 ClearForWrite
跳过（中 ROI） → 5.3 fast leaves（待定，需要 smoke profile）」。

现在 smoke profile 拿到了，§5.3 的待定项可以填实：

- smoke 上 hot forms 的 dispatch 频率非常集中（28 条 ALU body 是
  shifted-logical 和 add-immediate 两类的循环展开 + LDP/STP 收尾），
  做 fast-form leaves 能拿到很高的 hit rate。
- 但 smoke 的真正第一瓶颈不是 leaf 本体而是 dispatch overhead
  （33% on smoke, 6% on mixed）——所以光做 fast leaves 提速有限，
  得跟 dispatch 收缩配套做才能在 smoke 上拿到大头。

前一份 §6 把"30× native 在不上 JIT 前提下不可达"这个结论留下来。
这一份的数据强化了同样的结论：smoke 现在 ~29 cycles/insn，纯解释器
的物理底大约 6-10 cycles/insn（参看 §6），最多还有 ~3-5× 的空间。
继续超过这个就需要换路线（JIT 或者编译期专门化）。

## 6. 对 10×/原来的 baseline 目标的判断

baseline 是当前 cache 路径 39.55 / 6.49 ns/insn。10× 目标对应：

| Workload | 当前 | 10× 目标 | 估算需求 |
|---|---:|---:|---|
| mixed | 39.55 ns | 3.95 ns | ~18 cycle/insn，需要 NEON leaf 大重写 + dispatch 收缩 + JIT |
| smoke | 6.49 ns | 0.65 ns | ~3 cycle/insn，等于"PC 自增 + 一次 ALU"——只有 native 执行能达到，纯解释器物理不到 |

按 §5 的 lever 排列，**纯解释器下的现实上限**估算：

- mixed：NEON helper inline + ClearForWrite gate + heap removal + 部分
  pre-extract → 大致到 ~15-20 ns/insn，相对当前 **2-2.6×**。
- smoke：threaded dispatch + pre-extract operands + fast-form leaves +
  block dispatch → 大致到 ~2.5-3.5 ns/insn，相对当前 **1.9-2.6×**。

也就是说，**10× 在 no-JIT 约束下不可能**，2-3× 是可现实争取的范围。
这条信息会决定下一步开 OpenSpec change 的 scope：是按 2-3× 拆成
3-4 个增量 change（NEON helper inline 一个、dispatch 收缩一个、
operand pre-extract 一个），还是讨论放松 no-JIT 约束（比如允许
非-iOS 平台用 JIT，iOS 走解释器，这是另一条路线）。这一份只记数据；
方案讨论放在另一份。

## 7. 数据/方法的局限

- 单次 sampling，无重复。8566 / 6880 样本对前 20 项是稳定的，尾部
  ≤20 样本噪声较大。
- mixed 工作负载是 VIXL 生成器随机出来的，**不代表任何真实负载**。
  真实负载（zlib 解压、JSON parse、protobuf 编解码、Wasm 模块解释）
  的 NEON 占比大概率远低于这里的 68%。如果实际 embedder 工作负载
  几乎不碰 NEON，那 mixed 上 NEON 抽象优化的实际 ROI 会下降；
  smoke profile 反而更接近这类负载的瓶颈分布。
- `_platform_memset` / `_platform_memmove` 没拆调用源（要 call tree）。
  这一份只用了 top-of-stack 扁平计数。call graph 段在 `/tmp/*.txt`
  全文里，要细看可以直接 grep。
- smoke 的 5.4% bench harness overhead 是 **harness 自身行为**，不是
  simulator。要去掉这个噪声需要改 bench 的计时窗口（比如每 N 次
  RunFrom 才记一次 now()），但那是另一码事。

## 8. 索引

- 上一份 throughput 快照（含 cache + decoder 两个 engine 的 baseline）：
  [`baseline-benchmark-results-cache-2026-05.md`](./baseline-benchmark-results-cache-2026-05.md)
- 上一份 profile（mixed only）：
  [`gaby-vm-cache-hotpath-profile-2026-05.md`](./gaby-vm-cache-hotpath-profile-2026-05.md)
- cache 路径设计：
  [`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md)
- bench harness 说明：
  [`bench/README.md`](../../bench/README.md)
- 当前 `ExecuteInstructionCached` 实现：
  `src/aarch64/simulator-aarch64.h:1496-1575`
- raw profile dumps（local-only，未 commit）：
  `/tmp/mixed_cache_profile_2026_05_27.txt`、
  `/tmp/smoke_cache_profile_2026_05_27.txt`
