# Cache Hot-Path Profile + 优化 Roadmap — 2026-06-02

> Cache 路径的第三次定向 sampling profile，外加一份基于这次数据的优化
> roadmap。方法学跟前两份一致
> （[`2026-05`](./gaby-vm-cache-hotpath-profile-2026-05.md)、
> [`2026-05-27`](./gaby-vm-cache-hotpath-profile-2026-05-27.md)）：
> macOS `sample`，1 ms 间隔，profile build flags
> `-O3 -g -fno-omit-frame-pointer -DNDEBUG`。
>
> 这一份相对前两份的三个关键变化：
>
> 1. **mixed 的瓶颈结构整个换了。** 前一份 mixed 第一、第二热点是 NEON
>    format helpers（`LaneSizeInBitsFromFormat` 26%、`IsSVEFormat` 18%）。
>    `neon-format-helpers-constexpr-inline` / `neon-clearforwrite-and-helpers-inline`
>    两个 change 落地后，它们被 inline / constexpr 掉了，从 top-of-stack
>    彻底消失，mixed cache 也从 ~39.5 ns/insn 掉到 ~19.6 ns/insn（差不多砍半，
>    跟前一份 §6 的预测一致）。**现在 mixed 的新头号热点是 NEON 临时对象的
>    heap 分配 + memcpy/memset（~26%）**——完全不同的、而且更干净的靶子。
> 2. **新增「相对 native」这条标尺。** 这次用 `bench/native_baseline` /
>    `bench/native_smoke`（把同一段 committed workload 字节拷进 MAP_JIT 直接
>    在宿主 CPU 上跑）拿到了 native 基准，于是 cache 路径第一次有了一个
>    「慢多少倍」的绝对参照，并据此把「50× native」这个目标拆成可执行的
>    ns/insn 预算。
> 3. **dispatch 现在是两个 workload 各自的 top-of-stack 第一名函数。**
>    `ExecuteInstructionCached` 在 smoke 占 39%、mixed 占 12%——其它东西都
>    变便宜之后，dispatch 这条「每条指令的刚性税」相对占比反而升上来了。

## TL;DR

- **当前吞吐（dev-release）：mixed cache ~19.6 ns/insn（vs native ~359×），
  smoke cache ~6.45 ns/insn（vs native ~113×）。**
- **「50× native」是个按 workload 难度差很大的目标**：mixed 要 ~7.2× 提速、
  smoke 只要 ~2.26×。换算到 cycle（@4.4 GHz）：两边的 50× 目标都落在
  ~12–13 cycle/insn，正好压在纯解释器物理底（~6–10 cycle/insn）之上一点点。
  结论：**smoke 的 50× 在 no-JIT 下够得着，mixed 的 50× 不重写 NEON leaf
  基本到不了**（详见 §3、§6）。
- **瓶颈分布两个 workload 几乎正交，跟前两份的判断一致**：
  - mixed：NEON 临时对象的 heap + memcpy/memset ~26%、load/store leaf ~16%、
    dispatch ~13%、NEON/FP/format 残余 ~12%。
  - smoke：dispatch ~45%、ALU leaf（每次重抽操作数）~44%。
- **决策（见 §7）：两个 workload 都要、严格 no-JIT、接受按 workload 不同的
  落点；本轮只记录，暂不动代码。** 这份就是那个「记录」。

## 1. 测量设置

代码状态：commit `035f20b`（含到 `branch-hook-api` 为止的全部 cache /
NEON 优化）。机器：Apple M4 Pro，macOS 26.5，未做 pinning，P-core ~4.4 GHz。

```bash
cmake -S . -B build/profile -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGABY_VM_BUILD_BENCHMARKS=ON \
  -DGABY_VM_BUILD_NATIVE_BASELINE=ON \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -fno-omit-frame-pointer -DNDEBUG"
cmake --build build/profile --target bench_baseline bench_smoke native_baseline native_smoke

# mixed
./build/profile/bench/bench_baseline --mode cache --seconds 25 &
sample <pid> 10 1 -file /tmp/mixed_cache_profile_fresh.txt
# smoke
./build/profile/bench/bench_smoke --mode cache --seconds 20 &
sample <pid> 8 1 -file /tmp/smoke_cache_profile_fresh.txt
```

- 样本量（main thread，`gaby_vm::Simulator::RunFrom` 根帧计）：
  mixed **7851**、smoke **5351**。
- native 基准用 `bench/native_baseline` / `bench/native_smoke`——它们把
  **同一段 committed workload 字节**（mixed / smoke）拷进 MAP_JIT 可执行
  内存、直接在宿主 arm64 上跑，所以是真正同 workload 的 apples-to-apples
  参照，不是 pure-add 之类的代理。详见 [`bench/README.md`](../../bench/README.md)
  的 “Native baseline” 一节。

## 2. 当前吞吐 vs native（dev-release，每档 2 次取代表值）

mixed 64643 insn/pass，smoke 32 insn/pass。

| workload | native | cache | decoder | cache vs native | decoder vs native |
|---|---:|---:|---:|---:|---:|
| **mixed** | 0.0547 ns/insn | **19.6 ns/insn** | 118 ns/insn | **~359×** | ~2160× |
| **smoke** | 0.0572 ns/insn | **6.45 ns/insn** | 83 ns/insn | **~113×** | ~1450× |

- profile build（带 `-g -fno-omit-frame-pointer`）比 dev-release 慢约 3%，
  跟前一份观察一致；上面 §2 的表用 dev-release，§4/§5 的 sampling 占比用
  profile build。
- cache 相对 decoder：mixed ~6×、smoke ~13×——predecode/dispatch cache 本身
  的价值没变。

## 3. 「50× native」目标拆解

把 50× 翻成每条指令的时间预算和 cycle 预算（@4.4 GHz）：

| workload | 当前 cache | 50× 目标 | 需要提速 | 当前 cyc/insn | 目标 cyc/insn |
|---|---:|---:|---:|---:|---:|
| mixed | 19.6 ns | 2.74 ns | **7.2×** | ~86 | ~12 |
| smoke | 6.45 ns | 2.86 ns | **2.26×** | ~28 | ~13 |

读法：

- **为什么同一个「50×」两边难度差这么多。** native 把一条 guest
  `add v0.16b` 当**一条**硬件 SIMD 指令跑（~0.05 ns）；我们的解释器把它
  标量化成 16 路 C++ lane 循环。所以「50× native」隐含的要求是
  **每条 guest 指令平均 ~12 cycle**——对标量指令尚可争取，对一个 68% 是
  NEON 的 workload 近乎不可能（重 NEON leaf 本身就不止 12 cycle）。
- **smoke 的 50×（2.26×）够得着**：28 → 12.6 cyc/insn，正好落在前两份
  profile 估的「纯解释器现实底 ~2.5–3.5 ns / 11–15 cyc」区间的好那一端。
  靠 dispatch 收缩 + 操作数预解码 + fast-form leaves 有戏。
- **mixed 的 50×（7.2×）不重写 NEON leaf 到不了**：把它的 86 cyc/insn 砍到
  12 cyc，意味着每条（大量是 NEON 的）指令平均只花 12 cycle。即便把 NEON
  临时对象的 heap/memcpy 全干掉、dispatch 全收缩，也只能到 ~100–150× 量级；
  真要冲 50×，唯一现实路线是把 NEON/FP leaf 用**宿主 SIMD intrinsics** 重写
  （SIMD-on-SIMD，§6 Phase 4），那是一个大且语义高风险的工程。

> **目标再校准（重要，后补）**：这套「50× native」拆解是个 proxy。后续讨论把
> 真正该对标的尺子改成了「不慢于 Lua / JS（iOS app 内能用的解释器）」，并发现
> native baseline 高估了业务逻辑的 slowdown（这里的 113×/350× 是跟「4 IPC 理想
> native」比出来的；按真实业务逻辑的 native 算，标量的有效 slowdown 是 ~15–25×）。
> 详见 §8.2——读这一节结论时请连 §8.2 一起看。

## 4. Mixed fresh profile（7851 样本）

top-of-stack ≥ ~45 样本的项，归桶：

| 桶 | 样本 | % | 说明 |
|---|---:|---:|---|
| **NEON 临时对象 memory-mgmt**（`_platform_memmove` 883 + `_platform_memset` 632 + `__bzero` 92 + malloc/free 417） | **2024** | **25.8%** | `LogicVRegister`/`SimVRegister` 临时对象每条 NEON 指令 heap 分配 + 16 字节 lane 缓冲 memcpy/memset。call graph 显示 `ClearForWrite` 只是其中一部分，剩下是临时对象构造 + copy |
| load/store leaf（`LoadStorePairHelper` 298 + `LoadStoreHelper` 283 + `MemWrite` 137 + `Memory::Write` 128 + `StoreLane` 114 + `IsLoad` 105 + `CalcLS*DataSize` 128 + `LoadLane` 44） | ~1237 | ~15.8% | |
| **dispatch**（`ExecuteInstructionCached` 942 + `RunFrom` 90） | **1032** | **13.1%** | 每条指令的刚性税；现在是 mixed 的 top-of-stack 第一名单函数 |
| Visit\* 入口（VisitLogicalShifted / VisitMoveWideImmediate / VisitLoadStorePair\* / VisitBitfield / VisitNEON3Same / ...） | ~750 | ~9.5% | |
| NEON format/lane 残余（`LaneCountFromFormat` 177 + `LogicVRegister::Uint` 172 + `ClearForWrite` 本体 77 + `FPPairedAcross` 75 + ...） | ~640 | ~8.2% | 老的 `LaneSizeInBitsFromFormat`/`IsSVEFormat` 头两名已 inline 消失，这是残余 |
| FP（`SimulateFPRoundInt` 135 + `Float16ToRawbits` 95 + `RawbitsToFloat16` 94） | ~324 | ~4.1% | |
| scalar ALU leaf（`LogicalHelper` 168 + `AddSubHelper` 140 + `AddWithCarry` 44 + `ConditionalCompare` 46） | ~398 | ~5.1% | |

### 跟前一份（2026-05-27）的对照

| 类别 | 前一份 | 这一份 | 变化 |
|---|---:|---:|---|
| NEON format helpers（LaneSizeInBits / IsSVEFormat 等头部） | ~45.8% | ~8%（仅残余） | **inline/constexpr 干掉了头部** |
| NEON 临时 heap + memmove/memset | ~10% | **~25.8%** | 相对占比升（分母变小 + 没动这块） |
| dispatch | ~6.4% | ~13.1% | 相对占比升 |
| leaf helpers | ~20% | ~21%（load/store + ALU + FP 合计） | 基本不变 |

也就是说，前一份排在第一的 NEON 优化方向（format helper inline）**已经做完
并兑现了**，mixed 的下一块肉变成了「NEON 临时对象的生命周期」。

## 5. Smoke fresh profile（5351 样本，harness timer ~6% 另计）

| 桶 | 样本 | % | 说明 |
|---|---:|---:|---|
| **dispatch**（`ExecuteInstructionCached` 2107 + `RunFrom` 292） | **2399** | **44.8%** | 每条指令的刚性税；top-of-stack 第一名 |
| **ALU leaf**（`LogicalHelper` 1147 + `AddSubHelper` 1065 + `AddWithCarry` 168） | **2380** | **44.5%** | 每次调用还在从 `Instruction` 里重抽 Rn/Rm/Rd/shift/imm |
| Visit\* 入口（`VisitLogicalShifted` 428 + `VisitLoadStorePairPostIndex` 242 + `VisitUnconditionalBranchToRegister` 82） | ~752 | ~14.1% | |
| LS pair leaf（`LoadStorePairHelper` 302 + `CalcLSPairDataSize` 41 + `IsLoad` 39） | ~382 | ~7.1% | |
| bench harness timer（`mach_continuous_time` 199 + `clock_gettime*` + stubs） | ~310 | ~5.8% | **测量自身开销，不是 simulator**；smoke 单次 RunFrom 才 32 条，timer 频率跟吞吐同量级 |

> 注：top-of-stack 扁平计数对不同函数互斥，但 harness timer 帧落在
> `RunFrom` 之外（在 bench 计时循环里），所以各桶相加会略超 main-thread
> 根帧的 5351。这跟前一份的呈现方式一致。

`ExecuteInstructionCached` 这 39%（2107）是它**自己 inline 后的函数体**——
`LogAllWrittenRegisters` / `UpdateBType` / `IncrementPc` / movprfx 检查 /
form_hash 写 / pmf 加载都被 inline 进来了（leaf 调用本身另算成
LogicalHelper 等帧）。已确认 `LogAllWrittenRegisters` 在 cache 路径
（trace=0）只是三个 `if(ShouldTraceRegs())` not-taken 分支，不会去遍历
寄存器——所以这 39% 的大头是 **pmf 双重间接 + 每指令的固定 bookkeeping**，
不是隐藏的寄存器遍历。

## 6. 优化 Roadmap

按「两个 workload 都要、严格 no-JIT、接受差异化落点」的决策，lever 分四个
phase，按 ROI / 风险从低到高排。**每个 phase 各开一个 OpenSpec change，做完
量一次**（regression gate 已有：bench + `--hook` 变体）。

### Phase 1 — dispatch 收缩（共享，风险低）

打 `ExecuteInstructionCached` 的固定税（smoke 39% / mixed 12%）：

- **收缩虚函数 dispatch（thunk 表）**。`Visit*` 是 **virtual** 的，所以现在
  每条指令是一条 **3 层依赖 load 链**：load `entry->leaf` 指针 → load 16B 虚
  pmf（map 节点）→ load vtable（从 this）→ load fnaddr → `blr`。用 thunk 表把
  去虚化搬到编译期：thunk 体 `s->Simulator::VisitX(i)`（限定调用 = 非虚直接
  call），存 8 字节普通函数指针进 `entry->leaf`，运行期收成「1 层 load + 一次
  普通间接 call」。`PredecodedEntry` 仍 16 字节。**注意**：因为是虚函数，
  「抽 pmf word0 当代码地址」那条捷径不可行（word0 是 vtable offset）。完整
  机制、接线、收益校准见 §8.1。
- **movprfx 检查 gate 到 SVE-relevant flag bit**。现在每条指令算两次
  `"movprfx_z_z"_h` / `"movprfx_z_p_z"_h` 比较；挪到 predecode 阶段往
  `entry->flags` 写一个 bit，运行期只在该 bit 命中时才做（非 SVE workload
  永远不命中）。跟现有 BTI flag bit 同一个套路。
- **更紧的 block-dispatch 内层循环**。现在外层是
  `RunFrom` → `while(StepOnce())` → 每条都 `IsSimulationFinished()` +
  函数调用边界（`RunFrom` 自己占 smoke 5.5%）。改成 simulator 内部一个
  直线指令循环，减少每指令的 re-check 和调用边界。
- **（Phase 1.5）threaded dispatch——dispatch 的真天花板**。上面三项缩短的是
  每指令的固定延迟，但派发仍是**单个多态 `blr`**；对不规则代码 misprediction
  才是 dispatch 的大头，而这三项都不动它。治本是让每个 form 各自一个 `blr`
  （各自的分支预测历史），即 computed-goto / threaded code。这是更大、单独的
  一步，收益也更大；详见 §8.1 结尾。

预期（诚实，未实测）：thunk + movprfx + block-loop 三项合起来 smoke 大概低-到-
中个位数 % 到 ~1.2×，mixed 近乎无感。想要 §3 那种 smoke 往 50× 的量级，得靠
threaded dispatch（Phase 1.5）+ Phase 2 操作数预解码一起上。先按 spike 实测
thunk 这一项的真实数，别拍脑袋（§8.1）。

### Phase 2 — 操作数预解码 + fast-form leaves（共享，风险中）

打 leaf 本体（smoke ALU ~44%、mixed leaf ~20%）。这是设计文档
§4.2.1 / §4.6 推迟到 V2 的那块：

- predecode 阶段把热点 form（logical-shifted、add-sub、load-store-pair、
  move-wide）的操作数（Rn/Rm/Rd/shift/imm）抽出来存进 entry 旁路 side
  data；精简 handler 直接读预抽字段，省掉 `LogicalHelper`/`AddSubHelper`
  里每次的 bitfield 解析。
- **这不是新 IR**——只是把「decode 一次」做得更彻底（把操作数也 cache
  起来），符合 AGENTS.md「predecode once → cache → execute」的定位，不引入
  新的执行引擎。需要谨慎设计 side-data 存储（别破坏 16 字节 entry 的
  flat-array 性质）。

预期：smoke 往 50× 推进的主力。

### Phase 3 — NEON 临时对象生命周期（mixed only，风险中）

打 mixed 的新头号热点（heap + memcpy/memset ~26%）：

- 把 `LogicVRegister`/`SimVRegister` 临时对象从 heap 挪到栈 / 对象池，干掉
  per-NEON-insn 的 malloc/free（~5%）和大部分 memmove。
- `ClearForWrite` 在「指令本来就要整寄存器写满」时跳过 / 收窄 memset 宽度
  到实际 lane 宽（部分已做，还有空间）。

预期：mixed 在它最大那块上拿大头；对 smoke 无影响。

### Phase 4 —（stretch）宿主 SIMD NEON leaves（mixed only，风险高）

唯一能把 mixed 推向 50× 的路线：把 guest NEON/FP leaf 用宿主 NEON
intrinsics 重写（SIMD-on-SIMD），别再标量化成 lane 循环。工程量大、语义
（边界 / 饱和 / 舍入 / FP 异常）回归风险高，**可选**。不做的话 mixed 现实
落在 ~100–150×。

### 各 phase 落点估计

| phase | 主要受益 | 风险 | 预期落点 |
|---|---|---|---|
| 1 dispatch 收缩 | 两边（smoke 大） | 低 | smoke ~1.3–1.6× |
| 2 操作数预解码 + fast leaves | 两边（smoke 大） | 中 | smoke 向 ~50× |
| 3 NEON 临时对象 | mixed | 中 | mixed 砍掉 ~26% 那块 |
| 4 宿主 SIMD NEON | mixed | 高 | mixed 唯一的 50× 路线，否则止步 ~100–150× |

## 7. 决策记录

本轮两个 scoping 决策（AskUserQuestion）：

1. **50× 拿哪类 workload 当标尺** → 「两个都要，严格 no-JIT」。即：全部
   lever 都上，但接受 smoke 可能到 ~50×、mixed 现实大概只能到 ~100–150×
   的差异化落点；不放松 no-JIT（不走「非 iOS 平台 JIT」那条架构路线）。
2. **lever 推进顺序** → 「先只记录，暂不开工」。即：把这份 fresh profile +
   roadmap 落成文档先记下来，**这一轮不动优化代码**，等后续再决定先开哪个
   phase。

所以下一步不是写代码，是等指令开第一个 phase 的 OpenSpec change（大概率
从 Phase 1 dispatch 收缩起步，因为它现在是两个 workload 各自的第一名单
函数、风险最低、两边都受益）。

> 后补：后续讨论又把目标重新校准到「不慢于 Lua / JS」，并提出优化前先做一个
> 代表性业务逻辑的真机 head-to-head 基准（§8.2 的 Step 0）。所以真正的第一步
> 很可能是那个基准，而不是直接进 Phase 1。

## 8. 后续发现（同日 dispatch 深挖 + 目标再校准）

> 这一节是 profile 写完后、同日继续讨论挖出来的。两块：8.1 把 Phase 1 的
> dispatch lever 挖到底、纠正了一个判断；8.2 把整份 doc 的「50× native」目标
> 重新校准成「不慢于 Lua / JS」。

### 8.1 Phase 1 dispatch lever 深挖：它是个虚函数 dispatch

挖 `ExecuteInstructionCached` 的 pmf 调用时发现一个关键事实：**`Visit*` 是
virtual 的**（`simulator-aarch64.h:1756`：`#define DECLARE(A) virtual void
Visit##A(const Instruction*)`），`Simulator : public DecoderVisitor` 单继承。
这改了两件事：

1. **当前每条指令的成本比「pmf 双重间接」更重——是一条 3 层依赖 load 链。**
   `&Simulator::VisitX` 是 pointer-to-**virtual**-member，所以
   `(this->*pmf)(pc_)` 实际是：

   ```
   load entry->leaf            ; 指向 map 里 pmf 的指针
   load 16B pmf from there      ; HOP 1（map 节点）
        ; word0 低位置位 → 虚
   load vtable from *this        ; 额外 load
   load fnaddr from vtable+off    ; 额外 load
   blr fnaddr                    ; HOP 2
   ```

   即「pmf 从 map 取 + 完整虚分派（两次 vtable load）+ 间接 call」。

2. **「抽 pmf word0 当代码地址」这条捷径对虚函数不可行——之前的设想作废。**
   虚 pmf 的 word0 是 `vtable_offset + 1`（低位置位），不是代码地址，抽出来
   不能调；之前提的 `(& 1) == 0` guard 反而会 fire。记下来防止以后再踩。

**正确的收缩 = thunk 表，把去虚化搬到编译期。** thunk 体用**限定调用**：

```cpp
static void Visit_AddSub(Simulator* s, const Instruction* i) {
  s->Simulator::VisitAddSub(i);   // 限定 Simulator:: → 非虚 → 直接 call 到 override
}
```

`s->Simulator::VisitAddSub(i)` 绕过 vtable，编译成一条直接 tail-branch
`b Simulator::VisitAddSub`。它的**地址**是个 8 字节普通函数指针，存进
`entry->leaf`，运行期从「3 层 load 链 + 虚分派」收成「1 层 load + 一次普通
间接 call（到 thunk）+ 一条直接 branch」。

**接线干净、DRY（复用现有宏，不复制 form 表、不改共享宏）：** 关键是让
thunk-holder 的 `Visit*` 是 **static** 成员，这样 `&Holder::VisitX` 是普通
函数指针而不是 pmf：

```cpp
struct GabyLeafThunks {
#define GABY_THUNK(A) \
  static void Visit##A(Simulator* s, const Instruction* i) { s->Simulator::Visit##A(i); }
  VISITOR_LIST_THAT_RETURN(GABY_THUNK)
  SIM_AUD_VISITOR_LIST_THAT_RETURN(GABY_THUNK)
  VISITOR_LIST_THAT_DONT_RETURN(GABY_THUNK)
#undef GABY_THUNK
};
// 用同一张共享宏喂这个 struct，得到一张 cache 专用的「普通函数指针」表：
using LeafFnMap = std::unordered_map<uint32_t, void (*)(Simulator*, const Instruction*)>;
static const LeafFnMap m = { DEFAULT_FORM_TO_VISITOR_MAP(GabyLeafThunks) };
```

改动 4 处、都在 gaby-vm marker block：(1) `GabyLeafThunks` struct（~5 行宏
生成 ~几百个 1 行 thunk）；(2) cache 专用 `LeafFnMap` builder；(3)
`ResolvePredecodeLeaf` 返回函数指针值（8B）而不是 `&pmf`；(4) 调用点一次普通
间接 call。约 30–50 行。需确认 `Simulator::VisitX` 对 `GabyLeafThunks` 可见
（public 或加 friend）。现有共享 pmf map 原样保留给 disasm / auditor。

**收益校准（诚实，未实测）：** 把 3 层 load 链收成 1 层。收益取决于「这条依赖
链能不能被 leaf 执行盖住」——OOO 核会拿下一条指令的派发去跟当前 leaf 重叠：

- smoke 的 leaf 小（`LogicalHelper` / `AddSubHelper`），盖不住 → 看得见，大概
  低-到-中个位数 %。
- mixed 的 leaf 大（NEON），load 延迟早被盖住 → 几乎无感。

**它不动 dispatch 的真天花板。** 仍是**单个多态 `blr`** 派发点，它的
misprediction 不变；对不规则代码（mixed、真实分支多的负载）misprediction 才是
dispatch 的大头。治本要 **threaded dispatch**：每个 form 各自一个 `blr`、各自
的分支预测历史（computed-goto / threaded code），roadmap 记为 Phase 1.5，
是更大、单独的一步，收益也更大。**所以 thunk 表是便宜干净的清理，不是冲 50×
的主力**；要数就先跑 spike 实测 thunk 这一项，别拍脑袋。

> **后补（2026-06-02，同日继续）**：再往下聊发现 thunk + Phase 2 这套增量框法
> 其实是在给一个半成品打补丁——cache 今天只缓存了第一级派发（form→leaf），第二级
> 派发（leaf 里再 decode 选操作）和操作数抽取每次执行都在重做。真正想要的是换执行
> 模型：flat decoded entry（预抽 reg/imm + 简单 C 函数指针 leaf），最终去掉
> `form_hash_` 和 Simulator 虚继承，天然通向 threaded interpreter。这个方向的粗记
> 单独落在 [`gaby-vm-dispatch-redesign-notes-2026-06-02.md`](./gaby-vm-dispatch-redesign-notes-2026-06-02.md)。
> **当前不实施**：roadmap 的 Phase 1/2 搁置，近期先做更简单的事（大概率是 §8.2 的
> Step 0 基准）。

### 8.2 目标再校准：真正的标尺是 Lua / JS，不是 native

`50× native` 是个 proxy。对 iOS 热修这个真实用例，该对标的是**在 app 内真能
用的解释器**：

- **Lua（PUC 5.4）**：相对 native C ~15–45×。
- **JS（JavaScriptCore LLInt）**：app 内嵌 JSC 拿不到 JIT 权限（只有 Safari /
  WKWebView 那条特殊 entitlement 才有），只能跑 LLInt 解释器，数值代码
  ~15–60×，对象 / 属性更慢。

目标改成：**不慢于这俩，最好快一点。** 三个支撑这个目标可达的发现：

1. **native baseline 高估了业务逻辑的 slowdown。** smoke 的 native
   0.057 ns/insn = 4.4 GHz 下 4 IPC，是纯独立 ALU 合成负载的 ILP 峰值。真实
   业务逻辑（分支 / 依赖 / cache miss）native 跑 ~0.3–0.5 ns/insn。按真实 native
   算，gaby-vm 标量的**有效 slowdown 是 ~15–25×，不是 §2/§3 那个 113×**——也就
   是说今天大概已经追平 Lua、超过 in-app JS。§2/§3 的 113×/350× 是跟「4 IPC
   理想 native」比出来的，对业务逻辑虚高。
2. **gaby-vm 有结构性优势：跑的是编出来的真机器码，没有动态类型税**（无 tag
   check / 装箱 / GC）。指令多但每条便宜；Lua/JS 字节码少但每条重（动态分派 +
   类型检查）。对自定义算术 / 控制流 / 结构体操作，这俩大致抵消，甚至 gaby-vm
   占优。
3. **对手能赢的地方有解，而且解法是 gaby-vm 的强项。** Lua/JS 的重内置原语
   （字符串 / 字典 / 正则 / JSON / 排序）是 native C 实现、跑原生速度；gaby-vm
   逐条解释等价机器码会输。解法：**guest 对热点库函数的调用（`bl memcpy` /
   `bl malloc` / ...）用 native bridge 直接派发到宿主原生实现**（guest 是真机器
   码，这条路顺；项目已有 branch hook 雏形）。「精简解释器跑自定义逻辑 +
   native bridge 扛重原语」理论上能同时赢 Lua 和 JS。

**Step 0（优化前先做）：建一个代表性业务逻辑的真机 head-to-head 基准。** 现有
smoke（纯 ALU）/ mixed（68% NEON）都回答不了「vs Lua/JS」。需要选一段真实味儿
的业务逻辑（带分支和循环的校验 + 一点 struct 操作 + 一次小 JSON / 字段解析 +
几次原生调用），同一语义分别用 gaby-vm（跑编出的 ARM64）/ Lua 5.4 / JSC（强制
LLInt）在**真机**上量端到端 wall time。有了它，「不慢于 Lua/JS」才从口号变成能
盯着优化的硬指标，也可能一量就发现自定义逻辑已经赢了、优化重点应转向 native
bridge 覆盖重原语（ROI 比硬抠 dispatch 高得多）。这一步不写优化代码，只建测量
基准，跟「先量再压」一致。

> **后补（2026-06-08，已落地一半）**：Step 0 的 native 那条标尺已建成——
> `bench_business`，四个诊断性微核（parse/hash/struct/fsm），见
> [`gaby-vm-business-bench-2026-06-08.md`](./gaby-vm-business-bench-2026-06-08.md)。
> 首份数据的关键发现：**cache 是平的 ~6.5 ns/insn，slowdown 倍数（~19×–211×）由
> native 那侧的 IPC 决定，不是 gaby**。parse/fsm/hash 的 50× 够得着，struct（native
> ~6.8 IPC）是硬骨头。Lua/JS head-to-head 那一半仍未做。

## 9. 数据/方法的局限

- 单次 sampling，无重复。前 ~20 项稳定，尾部 ≤20 样本噪声大。
- mixed 是 VIXL 生成器随机出来的合成负载，**68% NEON 不代表任何真实负载**
  （zlib / JSON / protobuf 的 NEON 占比通常远低于此）。真实负载的瓶颈分布
  更接近 smoke。换句话说 mixed 的「50× 不可达」结论对应的是这个 NEON 极
  端合成负载；真实 embedder 负载若几乎不碰 NEON，反而更可能逼近 50×。
- memmove/memset 的 call tree 已抽样确认主要来自 `LogicVRegister` 临时对象
  （含 `ClearForWrite`），但没逐条拆全部调用源；要细看 call graph 在
  `/tmp/*_cache_profile_fresh.txt` 全文。
- native 基准的 dynamic path 跟 simulator 可能略有差异（初始寄存器值不同 →
  少数早期分支走向不同），但 committed workload 的分支会 reconverge，
  `iterations_per_second`（整段 workload 每秒跑几遍）是严格可比的指标。

## 10. 索引

- 前两份 profile：
  [`2026-05`](./gaby-vm-cache-hotpath-profile-2026-05.md)（mixed only）、
  [`2026-05-27`](./gaby-vm-cache-hotpath-profile-2026-05-27.md)（+smoke）。
- cache 路径设计：[`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md)
  （§4.2.1 / §4.6 是操作数预解码 / per-form thunk 的原始设计预留）。
- 当前 `ExecuteInstructionCached` 实现：
  `Sources/gaby_vm/src/aarch64/simulator-aarch64.h:1530-1609`。
- §8.1 涉及的代码位置（`Sources/gaby_vm/src/aarch64/`）：
  `simulator-aarch64.h:1756`（`Visit*` 的 `virtual` 声明宏）、
  `:1695`（`ResolvePredecodeLeaf`，返回 `&pmf`）、
  `:5780`（`FormToVisitorFnMap` typedef + pmf-size `static_assert`）、
  `decoder-visitor-map-aarch64.h`（`DEFAULT_FORM_TO_VISITOR_MAP` 共享宏）、
  `simulator-aarch64.cc:105`（`GetFormToVisitorFnMap` 的 map builder）。
- bench harness + native baseline 说明：[`bench/README.md`](../../bench/README.md)。
- raw profile dumps（local-only，未 commit）：
  `/tmp/mixed_cache_profile_fresh.txt`、`/tmp/smoke_cache_profile_fresh.txt`。
