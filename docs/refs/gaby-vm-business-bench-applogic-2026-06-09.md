# 给业务基准补上 FP/NEON：`applogic` 核 + 它改变了什么 — 2026-06-09

> **它是什么**：在 `bench_business` 的四个纯整数核之外，加了第五个核 `applogic`——
> 一个以正常业务逻辑为主、掺约 9% 标量 double FP 和少量 NEON 的「混合核」，外加它
> 跑出来的数据和一个值得记下来的发现。
>
> **为什么加**：前一份
> [`gaby-vm-business-bench-2026-06-08.md`](./gaby-vm-business-bench-2026-06-08.md)
> 的四个核全是 `-mgeneral-regs-only`（纯标量整数），这把真实 iOS 业务逻辑算窄了。
> iOS 上但凡碰 layout / 几何 / 动画的代码，主体都是标量浮点：CGFloat 在 64 位平台
> 就是 `double`，所以 Auto Layout 的约束求解、CGRect/CGAffineTransform、TextKit 的
> 行高基线、SwiftUI 的 layout pass，每一帧都在跑 `fadd/fmul/fdiv/fcmp/fcsel/fsqrt`
> 这一堆标量 FP；simd 类型和 Accelerate 还会带出少量 NEON。纯整数基准测不到这条最
> 常见的业务形态，于是给的 slowdown 数字偏乐观。

## TL;DR

- 新核 `applogic` 模拟「遍历一批 UI 元素、做布局再处理」：~90% 是其他四个核那种整数
  业务逻辑（字段 load/store、按元素分支派发、整数 digest），掺进 ~7% 标量 double FP
  做 CGFloat 风格的几何（仿射变换、min/max 钳制、面积、对角线 `fsqrt`）和 ~2% NEON
  做一个 4-lane 的打包矩形变换。实测动态混合 **FP 6.7% + NEON 1.9% = 8.6%**。
- 它是五个核里**唯一带 FP/NEON 的**，也是唯一不加 `-mgeneral-regs-only` 编的。
- **数据（M4 Pro，dev-release，每边两遍取均值）**：

  | 核 | 形态 | native ns/insn | cache ns/insn | **slowdown** | cache/decoder |
  |---|---|---:|---:|---:|---:|
  | hash | 整数依赖链 | 0.310 | 5.79 | ~19× | ~14× |
  | fsm | 分支密集 | 0.067 | 6.18 | ~92× | ~14× |
  | parse | 解码 | 0.060 | 6.59 | ~110× | ~13× |
  | struct | ILP 友好访存 | 0.035 | 7.24 | ~210× | ~13× |
  | **applogic** | **混合（含 FP/NEON）** | **0.030** | **10.02** | **~330×** | **~10×** |

- **一个值得记的发现：FP/NEON 把 gaby 那条「平线」抬起来了。** 前四个纯整数核的 cache
  成本卡在 ~6.5 ns/insn 一条近乎恒定的线上（2026-06-08 那份的核心结论）。applogic 只
  掺了 8.6% 的 FP/NEON，cache 成本就跳到 **10.0 ns/insn**——比纯整数高约 55%。原因是
  VIXL 的 FP/NEON leaf 比整数 leaf 重（要处理 NaN、舍入模式、更宽的操作数搬运），单条
  FP/NEON 指令的成本远高于一条整数指令。所以「gaby 是平的 ~6.5 ns/insn」这个结论，只
  在纯标量整数下成立；一旦掺进真实业务里就有的 FP，平线被抬高。

## 1. 核长什么样，自包含怎么保住

核本身不复杂：栈上用 LCG 现生成一批元素（id/kind/w/h/flags），48 轮遍历，每个元素先
跑整数业务（按 kind 四路分支算一个值、按 flags 异或/旋转、写回一个字段、累加 checksum），
然后约 1/8 的元素再做一段 CGFloat 风格的几何（把 w/h 经 `scvtf` 转 double，做 `1.5×+平移`
的仿射、取 min/max、算面积和 `fsqrt` 对角线、一个除法，结果 `fcvtzu` 量化回整数累加），
每 8 个元素做一次 4-lane 的 NEON 矩形变换。返回的 x0 当正确性 oracle。

真正的难点不是业务逻辑，是**怎么在引入 FP/NEON 的同时保住那个「零重定位」自包含闸门**
（gen 脚本用 `llvm-objdump -r` 把任何重定位判 FATAL，因为抽出来的 `.text` 要能直接被
native baseline 在宿主进程里 call）。去掉 `-mgeneral-regs-only` 之后踩到两个坑，都解决了：

- **浮点常数会落 literal pool。** AArch64 的 `fmov` 只能编码一小撮立即数（±1.0/1.5/2.0/
  0.5/1.25 这类）；任何别的 double 常数，编译器只能 `adrp+ldr` 从 `.rodata` 加载，那就是
  一条重定位。所以核里**不出现任何非平凡的硬编码浮点常数**：所有 FP 量要么是 `fmov` 可
  编码的良性常数，要么从整数 LCG 经 `scvtf` 转出来；钳制用两个运行时值比（`fminnm/fmaxnm`，
  不吃常数）。实测 `.text` 里没有 `.rodata` 段，零重定位。
- **`sqrt` 默认是 libcall。** `__builtin_sqrt` 在默认 flag 下会编成 `bl sqrt`（为了在负数
  输入时设 `errno`）——又是一条外部重定位。加 `-fno-math-errno` 后它编成单条 `fsqrt`，
  `-ffreestanding` 下本来也没有 `errno`，这个 flag 语义上不花钱。
- **NEON 不用 `arm_neon.h`**（`-ffreestanding` 不提供），用编译器的 `vector_size` 扩展 +
  `__builtin_convertvector`，splat 常数也用 `fmov` 可编码的 1.25f/0.5f，不落向量 literal pool。

gen 脚本因此改成 per-kernel flag：前四个核照旧加 `-mgeneral-regs-only`，`applogic` 换成
`-fno-math-errno`。生成 tag 里也相应记成 `flags=O2/fp+neon/no-jump-tables`，跟纯整数核区分。

## 2. 正确性闸门照常过

`bench_business --verify` 对 applogic 在 cache 轨（`StepOnce`）和 decoder 轨
（`DebugStepOnce`，参考 VIXL）各单步跑一遍，动态指令数和 x0 **逐位一致**
（dynamic=101532，x0=`0x00000414d717d726`）。这说明 FP/NEON 的 leaf 语义在 gaby 的两条
执行路径上跑出一样结果——这正是这个核要守的：在信任任何 FP 性能数字之前，先确认 cache
路径对带 FP/NEON 的代码跟参考 decoder 算得一样。五个核现在全过 `--verify`。

动态混合是单步跑一遍、对每个被执行的指令地址按助记符分类统计出来的（命中直方图 × 静态
反汇编分类），sum = 101532 跟 `--verify` 的动态数完全吻合。

## 3. 三个发现

### 3.1 gaby 的「平线」只在纯整数下成立

2026-06-08 那份说 cache 是近乎恒定的 ~6.5 ns/insn，跟业务形态无关。那是真的——**但前提
是纯标量整数**。applogic 掺了 8.6% FP/NEON，cache 就到 10.0 ns/insn。换算一下：那 8.6%
的 FP/NEON 指令，平均单条成本明显高于整数指令，把整体每指令成本拉高了约 55%。所以更准
确的说法是：**gaby 对整数是平的 ~6.5 ns/insn，对 FP/NEON 更贵**，真实业务的每指令成本落
在两者之间，取决于 FP 占比。

### 3.2 含 FP 的真实业务是新的最坏档（~330×），比 struct 还差

applogic 的 slowdown ~330×，超过原来最坏的 struct（~210×）。这是两头夹出来的：一头是
**native 在这个形态上很快**（0.030 ns/insn ≈ 6.8 IPC，跟 struct 一个档——整数主体 ILP
友好、FP 也流水得起来），另一头是 **gaby 在 FP 上更慢**（10.0 而非 6.5）。两个因素叠加，
让「代表性的、带 layout FP 的 iOS 业务」成为今天最难的一档。

意义很直接：之前用纯整数核测出的「最坏 ~210×」其实**低估了**真实情况。把 layout 这种最
常见的 FP 业务算进来，最坏档是 ~330×。用户定的「200× → 50×」目标里，真正要啃的就是这种
含 FP 的形态。

### 3.3 对优化 roadmap 的含义

- dispatch + 操作数预解码那套「打每指令固定税」的优化，对 applogic 仍然有效（它也付那条
  税），但 applogic 还多一块 **FP/NEON leaf 本身的成本**——压每指令固定税压不到 leaf 里
  去。要把含 FP 的业务也打进 50×，除了 dispatch 优化，可能还得看 FP/NEON leaf 路径本身有
  没有可收缩的地方（这是纯整数核完全暴露不出来的新方向）。
- 换句话说，applogic 给优化加了一个**整数核给不了的靶子**：先用 dispatch 优化把那条 ~10
  ns/insn 压一截，再看 FP leaf 是不是新的瓶颈大头。

## 4. 局限

- 单机、未 pinning，方差十几 %；前几位稳定。
- `applogic` 的 FP/NEON 占比（~9%）是按「以业务逻辑为主、掺少量 FP/NEON」的目标调出来的
  代表性值，不是抓某个真实 app 的指令直方图。形态对（整数主体 + layout FP + 一点 simd），
  但具体比例是设计选择。
- NEON 只占 ~1.9%，贴合「iOS 业务里向量化是少量」的实际；注意 Apple Silicon 没有 SVE 硬件，
  iOS 上向量化都落 NEON，所以这里不涉及 SVE。
- 跟前四个核一样，这是 native 这条标尺；「赢不赢 Lua/JS」要另一条 head-to-head 基准才能答。

## 5. 索引

- 触发这份的对话起点 + 四核首份数据：
  [`gaby-vm-business-bench-2026-06-08.md`](./gaby-vm-business-bench-2026-06-08.md)。
- 核源码：`bench/workloads/business/applogic.c`；生成流水线（per-kernel flag）：
  `bench/workloads/business/gen_business_workloads.sh`；harness 说明见
  [`bench/README.md`](../../bench/README.md)。
