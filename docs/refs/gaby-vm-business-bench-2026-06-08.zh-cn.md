# 业务逻辑基准 `bench_business` + 首份数据 — 2026-06-08

> **它是什么**：一个贴合「iOS 热修业务逻辑」场景的新基准，外加它跑出来的第一份
> cache-vs-native 数据和解读。源头是
> [`gaby-vm-cache-hotpath-profile-2026-06-02.md`](./gaby-vm-cache-hotpath-profile-2026-06-02.md)
> §8.2 的 **Step 0**——那一节说现有 smoke（纯 ALU、太简单）和 mixed（68% NEON、不
> 代表真实负载）都回答不了「在真实业务逻辑上慢多少」，要先建一个代表性基准把尺子
> 立起来，再谈优化。这份就是把那个 Step 0 落地（先做 native 这条标尺；Lua/JS
> head-to-head 留作后续）。
>
> **它不是什么**：不是优化本身。这一轮只建基准 + 量基线，不动执行路径的代码。

## TL;DR

- 建了 `bench_business`：**四个诊断性微核**，编译 C 出来的自包含、纯标量（无
  NEON / 无 syscall / 无外部调用）ARM64 函数，各自模拟一种业务形态，逐核报数。
  配套 `native_business` 把**同一份字节**拷进 MAP_JIT 在宿主 arm64 上跑，当诚实
  分母。`--verify` 用 cache / decoder 双轨对拍同一个 x0 结果当基准自身的正确性闸门。
- **首份数据（M4 Pro，dev-release，每边两遍取均值）**：

  | 核 | 业务形态 | native ns/insn | cache ns/insn | cache cyc/insn @4.4G | **slowdown** | cache/decoder |
  |---|---|---:|---:|---:|---:|---:|
  | **hash** | 整数密集、依赖链绑死 | 0.31 | 5.98 | ~26 | **~19×** | 13.1× |
  | **fsm** | 分支密集扫描器 | 0.066 | 6.20 | ~27 | **~94×** | 14.1× |
  | **parse** | 分支密集解码 | 0.061 | 6.50 | ~29 | **~108×** | 12.9× |
  | **struct** | ILP 友好 load/store | 0.034 | 7.14 | ~31 | **~211×** | 12.7× |

- **三个核心结论**：
  1. **gaby cache 是近乎恒定的 ~6.5 ns/insn（~28 cyc），跟业务形态几乎无关。** 五种
     差异巨大的负载，per-insn 成本只在 5.98–7.14 之间晃。瓶颈是**每条指令的刚性税
     （派发 + 每次执行重抽操作数）**，指令具体干啥（parse 的分支、hash 的乘法、
     struct 的访存）只是上面一层零头。跟前几份 profile 的 smoke 6.45 ns/insn 一致。
  2. **slowdown 倍数完全由 native 那一侧（native 在该形态上的 IPC）决定，不是 gaby
     决定的。** struct 211× 不是因为 gaby 跑 struct 慢，是因为 native 跑 struct 到
     ~6.8 IPC（0.034 ns/insn）；hash 只有 19× 是因为 hash 的依赖链把 native 也卡住了
     （0.31 ns/insn）。同一个 gaby，倍数差 11 倍，差的全是分母。
  3. **优化的杠杆是那个恒定的 ns/insn，而「50×」能不能够到，取决于具体业务形态的
     native IPC。** 见 §5。

## 1. 为什么要这个基准

目标是把 gaby 解释执行从「比 native 慢约 200×」提升到「50×」，但前提是有一把**量得
准的尺子**。现有两个基准都不合用：

- **smoke**：32 条直线 ALU，太简单，timer 开销占 ~6%，且只有一种形态。
- **mixed**：VIXL 生成器随机合成，**68% 是 NEON**——iOS 业务逻辑（zlib/JSON/protobuf
  那类）的 NEON 占比远低于此。它的瓶颈分布（NEON 临时对象 heap、SIMD lane 循环）
  对业务逻辑没有代表性。

而且——这是最关键的一点——**「200×」这个数字本身依赖于拿什么 native 当分母**。前一份
profile §8.2 已经发现：smoke 的 native 0.057 ns/insn 是「4 IPC 理想峰值」，拿它当分母
会把 slowdown 算虚高。所以新基准必须用**同一段业务逻辑编出的真机器码在宿主 CPU 上跑**
当分母（现有 native baseline 工具正好干这个），而不是合成 ALU 的 ILP 峰值。

## 2. 四个微核

每个核是一个独立的 C 函数（`bench/workloads/business/<name>.c`），编译成自包含、AAPCS
平衡的 ARM64 `.text`，签入成 `uint32_t[]` header。选诊断性微核（而不是一个混合大核）
是因为目标是**驱动优化**：逐核报数能直接告诉我们解释器在哪种业务形态上慢。

| 核 | 模拟 | 压力点 | 诊断定位 |
|---|---|---|---|
| **parse** | 序列化记录解码（varint/TLV wire） | 数据相关分支、字节 load、varint 续位循环、wire-type 派发 | 贴近「解码 payload」；分支密集 → 验证单-`blr` 派发天花板 |
| **hash** | 整数 digest（FNV-1a + avalanche 混合） | 乘/移/异或紧循环、几乎无分支、寄存器驻留 | 解释器**最好情况**；标量上限参照 |
| **struct** | struct 数组变换（读字段、算派生、条件写回） | 带 offset 的 load/store、指针算术、混合分支 | load/store leaf 路径；**native 最快**的形态 |
| **fsm** | 分支密集状态机扫描器（tokenizer 风格） | 不可预测的按 state+char-class 双重派发 | 解释器**最坏情况** → 最直接论证 threaded dispatch |

**自包含契约**（四个核共用，由编译 flag 强制保证）：

- 无外部调用、无 `.rodata` 引用（输入数据由核内 LCG 现生成；switch 编成比较/分支链而
  非跳转表）。这样抽出来的 `.text` 是一个零重定位的单函数，**既能在 simulator 里跑、
  也能当 native baseline 直接调用**。
- 所有可写内存走栈（≤2 KB，远小于 `kMinStackSize` 12 KB）。
- 确定性：同种子 → 同路径 → 每次 pass 同结果（x0 当 oracle）。
- 编译 flag：`--target=aarch64-linux-gnu -O2 -mgeneral-regs-only -fno-jump-tables
  -fno-builtin -ffreestanding -ffixed-x18`。`-mgeneral-regs-only` 硬保证无 NEON/FP，
  贴合「不含 cpu dense、不含 NEON」的场景；`-fno-jump-tables` 保证 switch 不落
  `.rodata`；**`-ffixed-x18` 禁止分配 x18**——Linux triple 会把 x18 当 scratch，但
  Apple 平台（macOS 和 **iOS**）保留 x18 作平台寄存器。native baseline 在宿主进程里
  直接执行这些字节，用 Darwin 保留寄存器会让分母 ABI-dependent；更要紧的是真实 iOS
  业务 codegen 根本不碰 x18，用了它的 workload 就不代表场景。`-ffixed-x18` 保住 ELF
  抽取流水线的同时对齐 Apple ABI。（去掉它时 `parse` 会分配 x18 共 14 次；加上后纯属
  寄存器重命名——动态计数 / x0 / cache 数字全不变，native 在噪声内。）

生成流水线见 `bench/workloads/business/gen_business_workloads.sh`：编译 → `llvm-objdump
-r` 验证零重定位（自包含闸门）→ `llvm-objcopy` 抽 `.text` → 生成 header（只含字节 +
静态字数 + tag）。动态指令数和 expected x0 需要运行期 simulator，由 `bench_business
--verify` 单步测出，放在**手维护的 `oracle_data.h`**（gen 脚本永不触碰）——这样重生成
只刷新机器产物，绝不会把 oracle 常量悄悄清零。

## 3. 正确性：cache / decoder 双轨对拍

`bench_business --verify` 对每个核，在 cache 轨（`StepOnce`）和 decoder 轨
（`DebugStepOnce`，即参考 VIXL 实现）上各单步跑一遍，对拍最终 x0 和动态指令数。四个核
**逐位一致**：

```
parse : dynamic=156172  x0=0xc196940487fb10c0   cache==decoder ✓
hash  : dynamic=32856   x0=0x76dd61c78f2a67e3   cache==decoder ✓
struct: dynamic=47827   x0=0x000005cf4397d892   cache==decoder ✓
fsm   : dynamic=667366  x0=0x72279a557be93af8   cache==decoder ✓
```

这是 ShadowRunner oracle 的同款思路，用在基准自身上：在信任任何性能数字之前，先确认
cache 路径对真实业务逻辑跟参考 decoder 跑出一样的结果。

## 4. 测量设置

```bash
cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON -DGABY_VM_BUILD_NATIVE_BASELINE=ON
cmake --build --preset dev-release --target bench_business native_business

./build/release/bench/native_business --seconds 1.0     # 宿主 arm64，mode: native
./build/release/bench/bench_business --mode cache --seconds 2.0   # gaby 解释器
./build/release/bench/bench_business --mode decoder --seconds 1.0 # 参考 VIXL 轨
```

机器：Apple M4 Pro，macOS 26.5，未做 pinning，P-core ~4.4 GHz，dev-release（Release）。
每边跑两遍取均值；run-to-run 方差十几 %（见 README 的 Host hygiene）。primary 指标是
`iterations_per_second`（整段 workload 每秒跑几遍），跨 native/cache/decoder 严格可比；
`ns_per_instruction` 用签入的动态计数换算。

## 5. 核心解读

### 5.1 gaby 是平的，分母是斜的

把数据拆开看最清楚：**cache 这一列几乎是常数（5.98–7.14 ns/insn），native 这一列横跨
一个数量级（0.034–0.31 ns/insn）**。slowdown 倍数 = cache / native，所以倍数的全部
方差来自 native。

- **struct（211×）**：native 0.034 ns/insn ≈ 6.8 IPC。struct 的内层循环短、跨记录高度
  并行，M4 的宽 OOO 核几乎打满。native 太快 → 倍数最大。**不是 gaby 慢，是 native 快。**
- **hash（19×）**：native 0.31 ns/insn。FNV + avalanche 是一条串行依赖链（每步
  `h = h*prime; h ^= h>>33; ...` 都依赖上一步，乘法 3–4 cyc 延迟），native 也流水不
  起来。分母被卡住 → 倍数最小，**gaby 在这种形态上今天就已经是 19×**。
- **parse（108×）/ fsm（94×）**：native ~0.06 ns/insn（分支密集但 native 分支预测器
  扛得住，~3.5 IPC）。gaby 付恒定 ~6.3 ns/insn → ~100×。

### 5.2 那「50×」到底要怎么够

既然 gaby 是平的，**把恒定的 ns/insn 压下去，所有核的倍数等比例下移**。前一份 profile
roadmap 给 smoke 的 50× 目标是 6.45 → 2.86 ns/insn（约 2.27× 提速，靠 dispatch 收缩 +
操作数预解码 + threaded dispatch）。把这个提速套到这四个核：

| 核 | 今天 | 假设 cache→2.86 ns/insn 后 | 够到 50×？ |
|---|---:|---:|---|
| hash | 19× | ~8× | ✓ 远超 |
| fsm | 94× | ~42× | ✓ |
| parse | 108× | ~47× | ✓（贴边） |
| struct | 211× | ~93× | ✗ |

**结论：对 parse / fsm / hash，50× 在 no-JIT 下够得着**——就是 roadmap 那套
per-instruction 优化（dispatch + 操作数预解码 + threading）的直接收益。**struct 是真正
的硬骨头**：native 在它上面已经 ~6.8 IPC，no-JIT 解释器再怎么压每指令成本也追不平一个
打满流水线的乱序核。任何「最大 ILP 友好」的业务代码都会落在 struct 这一档。

换句话说，用户定的「200× → 50×」里的 200× 对应的正是 struct 这种最坏形态。它能不能到
50× 取决于优化能把 ns/insn 压多低——但更现实、ROI 更高的目标是**把代表性业务形态
（parse/fsm/hash 这类带分支、带依赖的真实逻辑）打进 50×**，这是够得着的；struct 那种
ILP 极端值作为上界单独看待。

### 5.3 对优化 roadmap 的含义

- **这把尺子证实了 roadmap 的方向是对的，且收益是普适的。** 因为 gaby 是平的，
  dispatch + 操作数预解码这类「打每指令固定税」的优化对**所有**业务形态等比例见效，
  不像 NEON 优化只对 mixed 有用。
- **per-insn 成本（~6.5 ns）里，dispatch 和操作数重抽各占大头**（profile §5 在 smoke
  上测到 dispatch ~45% / ALU leaf 重抽 ~44%）。fsm 是分支最不可预测的核（94× 且
  native 已经 3.5 IPC），它最能放大「单个多态 `blr` misprediction」这个 dispatch 真
  天花板——**threaded dispatch（每个 form 各自的分支预测历史）应该在 fsm 上收益最大**。
  这给了 [dispatch 重设计](./gaby-vm-dispatch-redesign-notes-2026-06-02.md) 一个具体的
  验证靶子：第一刀切下去，先看 fsm/parse 的 ns/insn 掉没掉。
- **下一步量化建议**：先跑 Phase 1（dispatch 收缩 / thunk 表）的 spike，用这四个核
  量 ns/insn 的真实变化，再决定要不要上更大的 threaded dispatch / 操作数预解码。

## 6. 局限

- 单机、未 pinning，方差十几 %；前几位稳定。
- 四个核是**合成**的代表性形态，不是某个真实 app 的实际热点函数。形态对（分支/依赖/
  访存/整数密集四种），但具体指令混合是编译器对这四段 C 的输出，不是抓真实业务 trace。
- **只做了 native 这条标尺。** §8.2 真正的对标尺是「不慢于 Lua / JS（app 内能用的
  解释器）」；那条 head-to-head（同语义在 Lua 5.4 / JSC-LLInt / gaby 上真机比 wall
  time）还没做。native slowdown 是中间量：它告诉我们 per-insn 成本和上界，但「赢不赢
  Lua/JS」要那条基准才能回答。
- native 的 dynamic path 跟 simulator 可能极少数早期分支不同，但四个核都是确定性自生成
  数据，路径与初始寄存器无关，所以 `iterations_per_second` 严格可比。
- `struct` 的 native 0.034 ns/insn ≈ 6.8 IPC 偏高但对 M4 的宽核 + 短并行循环可信；它代表
  ILP 上界，不代表「典型」业务代码。

## 7. 索引

- 触发这份的 profile + §8.2 Step 0：
  [`gaby-vm-cache-hotpath-profile-2026-06-02.md`](./gaby-vm-cache-hotpath-profile-2026-06-02.md)。
- dispatch 换模型的粗记（threaded interpreter 方向）：
  [`gaby-vm-dispatch-redesign-notes-2026-06-02.md`](./gaby-vm-dispatch-redesign-notes-2026-06-02.md)。
- 基准源码：`bench/business.cc`、`bench/native_business.cc`、
  `bench/workloads/business/{parse,hash,struct,fsm}.c` + 生成脚本
  `gen_business_workloads.sh`；harness 说明见 [`bench/README.md`](../../bench/README.md)。
