## Context

Cache 路径 profile (`docs/refs/gaby-vm-cache-hotpath-profile-2026-05-27.md`)
显示 mixed workload 上一小撮 NEON format helpers 占 ~46% 时间：

| 函数 | 占比 | 现位置 |
|---|---:|---|
| `LaneSizeInBitsFromFormat` | 26.3% | `src/aarch64/instructions-aarch64.cc:1304-1337` |
| `IsSVEFormat` | 17.7% | `instructions-aarch64.cc:1200-1212` |
| `LaneCountFromFormat` | 1.4% | `instructions-aarch64.cc:1378-1404` |
| `RegisterSizeInBytesFromFormat` | 0.3% | `instructions-aarch64.cc:1299-1301` |
| `LaneSizeInBytesFromFormat` | 0.2% | `instructions-aarch64.cc:1340-1342` |

每个 helper 是 `VectorFormat`（6-bit 枚举）上的纯 switch，返回 8/16/32/64
或 bool。定义在 `.cc` 里 → 每次调用都是一个 externally-linkable function
call + switch + return。NEON leaf 一条指令调它们 5-10 次（lane count、
lane bits、format 类型判断、`ClearForWrite` 传入...），叠出来就是上面
这个占比。

把它们搬进 header 标 `constexpr inline`，可以让编译器在调用点：

- 如果 `VectorFormat` 是 compile-time constant（绝大多数 NEON leaf 的
  情况——`Simulator::add(kFormat4S, ...)` 这样的字面量），整个函数折
  成立即数。
- 如果 `VectorFormat` 是 runtime variable（比如从 `Instruction` 解出来），
  仍然能省掉 function call overhead，switch 体本身被 inline 进调用点
  跟周围代码一起优化。

VIXL boundary 约束（`docs/architecture.md` §"Marker convention"）：
修改 imported VIXL 文件必须用 `// gaby-vm BEGIN:` … `// gaby-vm END`
标块，token 全小写，`git grep -nE 'gaby-vm( BEGIN| END|:)'` 要能枚举所
有 drift。原 `.cc` 里被搬走的函数体位置也要留 marker，说明"该函数定
义已上移至 header"，方便审计。

## Goals / Non-Goals

**Goals:**

- 让 mixed cache 路径上 NEON format helpers 的成本从 ~46% 砍掉一大半
  （目标 ~25-28 ns/insn from 39.55 ns/insn，约 **1.4-1.6×**）。
- 维持 imported VIXL 的 observable 语义不变：函数签名、namespace、调
  用关系、`VIXL_ASSERT`/`VIXL_UNREACHABLE` 行为零变化。
- 维持 VIXL boundary 审计能力（marker block 覆盖每个 drift 点）。
- 不引入任何新的公共 API、不动 `include/gaby_vm/`。

**Non-Goals:**

- 不优化 `LogicVRegister::ClearForWrite` 本体（lever B，独立 change）。
- 不优化 NEON leaf 内部的 heap allocation（lever C，独立 change）。
- 不动 `ExecuteInstructionCached` 或 `PredecodedEntry` (lever D/E)。
- 不重命名、不重组 NEON helpers 的命名空间或类型签名。
- 不把整个 `instructions-aarch64.cc` 里所有 helper 都搬到 header——
  只覆盖 profile 里实测占比显著的那一组（见 Decisions §1）。
- 不引入 GCC/Clang 特有的 inline attribute（`always_inline` 等）；标
  准 `constexpr inline` 已经足够 + 跨编译器可移植。

## Decisions

### 1. 要搬的函数：6 个，不是 5 个

原始 profile 列出的是 5 个 helper。实际依赖图要求带上一个 trampoline：

```
RegisterSizeInBytesFromFormat ──> RegisterSizeInBitsFromFormat ──> IsSVEFormat
LaneSizeInBytesFromFormat ───────> LaneSizeInBitsFromFormat
LaneCountFromFormat (no dep)
IsSVEFormat (no dep)
LaneSizeInBitsFromFormat (no dep)
```

如果只 inline `RegisterSizeInBytesFromFormat` 但留 `RegisterSizeInBitsFromFormat`
在 `.cc`，外层 inline 没意义——内层仍然是 external call。所以**最小
完整闭包是 6 个函数**：

1. `IsSVEFormat(VectorFormat)`
2. `LaneSizeInBitsFromFormat(VectorFormat)`
3. `LaneSizeInBytesFromFormat(VectorFormat)`
4. `LaneCountFromFormat(VectorFormat)`
5. `RegisterSizeInBitsFromFormat(VectorFormat)`
6. `RegisterSizeInBytesFromFormat(VectorFormat)`

加上一个 `RegisterSizeInBitsFromFormat` 不构成 scope creep——它是为
了让第 6 个有意义的必要依赖。Profile 没把它单独列在 top-of-stack
（占比 <0.2%），但它是 NEON leaf 的间接 cost 通道。

**不**搬的同族函数（保持在 `.cc`）：

- `LaneSizeInBytesLog2FromFormat`：profile 里看不到（<0.1%），等需要时
  再做。
- `MaxLaneCountFromFormat`：profile 里看不到。
- `SVEFormatFromLaneSizeIn*` 系列：profile 里看不到，且依赖关系更深
  （内部 trampoline 一层），暂不动。
- `ScalarFormatFromFormat`：profile 里看不到；它内部已经调 inline 之
  后的 `LaneSizeInBitsFromFormat`，会自然受益。
- `IsVectorFormat`、`MaxIntFromFormat` 等：profile 里看不到，单独评估
  再说。

**理由**：profile 数据驱动 scope，避免一次性把 instructions-aarch64.cc
里整批 helper 都搬走那种 sweeping refactor。每次 landing 测一次，看
真实收益再决定下一刀。

### 2. `constexpr inline` 而不是 `inline` 或 `static inline`

C++17 之后 `constexpr` 自动隐含 `inline` linkage（多 TU 同定义安全）。
但显式写 `constexpr inline` 有两个好处：

- 显示告诉读代码的人"这是定义而非声明"——header-only 函数体在跨
  TU 复用，意图不会被误读。
- 对 GCC/Clang，`constexpr` 触发 SFINAE-friendly constant-folding：
  如果调用方 `static_assert(LaneSizeInBitsFromFormat(kFormat4S) == 32, ...)`
  能编译期通过，证明 inline 生效。可作为编译期回归保护。

**为什么不**用 `static inline`：会让每个 TU 各持一份独立 symbol，最坏
情况二进制 size 增加（实测影响应该忽略不计，但 `constexpr` 是 C++17
习惯做法，无理由偏离）。

**为什么不**用 GCC/Clang 的 `always_inline` attribute：编译器在 `-O3` 下
默认会 inline 小 switch 函数；强制 `always_inline` 一般用于 hot loop
unrolling，这里不需要。可移植性也是考虑：标准 `constexpr inline` 在
MSVC、GCC、Clang 上语义一致。

### 3. 函数体里的 `VIXL_ASSERT` / `VIXL_UNREACHABLE` 在 inline 之后行为

这些 helper 内部含两类校验：

- `VIXL_ASSERT(vform != kFormatUndefined)` (在 `LaneSizeInBitsFromFormat`
  / `RegisterSizeInBitsFromFormat` / `LaneCountFromFormat` 起手)
- `VIXL_UNREACHABLE()` (default case of switch)

这两个宏在 `globals-vixl.h` 里定义：

- Release build (`NDEBUG`)：`VIXL_ASSERT` 是 no-op；`VIXL_UNREACHABLE`
  是 `__builtin_unreachable()` 或等价的 hint，告诉编译器 default 不可
  达，进一步帮助 inline 优化。
- Debug build：`VIXL_ASSERT` 是真断言，`VIXL_UNREACHABLE` abort。

Inline 不改变这两个宏的行为——它们本来就是 macro，不是 function call。
搬进 header 之后 macro 在调用点照常展开。**完全行为保留**。

### 4. Marker block 怎么布置

**Header (`instructions-aarch64.h`)** 在现有的函数声明位置（行 784-791）：

```cpp
// gaby-vm BEGIN:
// 下面 6 个 VectorFormat helper 是从 instructions-aarch64.cc 上移过来的
// constexpr inline 定义。改动原因：解释器 cache 路径 mixed workload
// profile (docs/refs/gaby-vm-cache-hotpath-profile-2026-05-27.md) 显示
// 这一组 helper 单独占 ~46% 执行时间，每条 NEON leaf 调它们 5-10 次；
// inline 之后编译器在调用点可以折成立即数。函数体逐字保留，未修改
// switch 内容或 assert 行为。原 .cc 位置留 marker 指回此处。
constexpr inline bool IsSVEFormat(VectorFormat vform) {
  // ...
}
// ... 其余 5 个 ...
// gaby-vm END
```

**Source (`instructions-aarch64.cc`)** 在原函数体位置：

```cpp
// gaby-vm BEGIN:
// 6 个 VectorFormat helper 的定义已上移至 instructions-aarch64.h
// 作为 constexpr inline，理由见该 header 对应 marker block。
// 此处刻意保留 marker 以记录搬迁点，避免未来 git blame 时找不到原
// 位置。
// gaby-vm END
```

不在 .cc 留任何 placeholder 函数体——这会触发 ODR 违规（header 已
经有定义）。marker 块仅为审计/grep 锚点。

### 5. 函数签名的 `constexpr` compatibility check

逐函数验证 body 是 constexpr-eligible（C++17 relaxed constexpr）：

- 只包含 switch、return、function call to other constexpr functions、
  macro expansion (VIXL_ASSERT / VIXL_UNREACHABLE)
- 不包含：try/catch、throw、动态分配、static-local variable、虚函数
  调用、reinterpret_cast、修改非 mutable 成员

6 个函数全部满足。

**`VIXL_ASSERT` 在 constexpr 上下文中**：release build 下是 `(void)0`，
trivially constexpr。debug build 下，VIXL 的 macro 展开是
`if (!cond) Abort()` 形式——`if` 在 C++17 constexpr 允许，`Abort()` 不
是 constexpr。**但只在运行期 assertion fails 时才被 evaluated**，所
以 constexpr 评估时（`vform != kFormatUndefined` 在 compile-time true）
不影响。如果某个 caller 真用 `kFormatUndefined` 在 constexpr context
调，会触发 constexpr error，这是想要的行为（编译期捕获语义 bug）。

**`VIXL_UNREACHABLE()` 在 constexpr 上下文中**：展开成
`__builtin_unreachable()`，是 GCC/Clang intrinsic，**不是 constexpr**。
但同样只在 default case 命中时被 evaluated；constant-folded 的调用永远
不命中 default，所以不影响 compile-time folding。

风险：如果将来 VIXL 改 `VIXL_UNREACHABLE` 的展开方式，可能需要再评
估。当下 OK。

### 6. 不在这一刀做 force-inline 调用站点

NEON leaf 内调 `LaneSizeInBitsFromFormat` 的位置，是否还需要在 leaf
那一侧加 `[[gnu::always_inline]]` 之类 attribute 把 inline 推到底？

**不做**。`-O3` 下 clang/gcc 对小 constexpr 函数的 inline 决策足够积
极，profile 应该能直接看到收益。如果 landing 之后 mixed cache ns/insn
没变到预期（~25-28），再考虑下一刀加 attribute——但那应该是单独的
change，不混在这一刀里。

## Risks / Trade-offs

- **风险**：constexpr inline 之后某个 caller 触发 `VIXL_UNREACHABLE`
  default case，行为跟之前不同。
  → Mitigation：`VIXL_UNREACHABLE` 在 release 是
    `__builtin_unreachable()` (UB hint)，跟原 `.cc` 定义里的行为一致。
    Debug 是 abort，也跟原行为一致。**Behavior 字节级保留**。

- **风险**：编译时间增加（header 体积变大 + 每个 TU 都 inline 解析）。
  → Mitigation：6 个函数总体积 ~80 行 switch，对 instructions-aarch64.h
    的现有 ~2k+ 行规模来说边际增加。bench 一次重建监控是否有显著 build
    time regression（通常 <5%）。

- **风险**：二进制 size 变化（每个 leaf 调用点都 inline 一份 switch）。
  → Mitigation：编译器对 switch-only 函数的常量传播之后，绝大多数 leaf
    调用点会被折成立即数 / no-op，反而 size 可能下降。如果不下降，最坏
    case 也是 +几 KB，可以接受。tasks.md 会要求记录 release build 二进制
    size delta。

- **风险**：constexpr 之后某个 caller 在 constexpr-context 调用，导致
  之前没出错的代码现在编译失败。
  → Mitigation：VIXL imported caller 全是 runtime 调用（leaf 里），
    没有 `static_assert(LaneSizeInBitsFromFormat(...) == ...)` 这种用法。
    gaby_vm 自己也没用过这一组 helper 在 constexpr context。grep 确认。
    新增 constexpr 约束只是"upgrade"，不破坏 runtime 调用。

- **Trade-off**：这一刀只覆盖 6 个 helper，profile 里另几个 NEON 相关
  helper（`LaneSizeInBytesLog2FromFormat`、`MaxLaneCountFromFormat`、
  `SVEFormatFrom*` 系列）暂不动。
  → 理由：这次想验证"profile-driven 小改"的收益曲线。如果 6 个搬完
    mixed 收益符合估算，再开下一个 change 覆盖其余。如果收益不如预
    期，那些 helper 的搬迁更不会有显著收益，可以省了。

## Migration Plan

无 runtime migration——纯 compile-time 改动。embedder 重新编译会自动
拿到新 binary。无 ABI breakage。

回滚策略：单次 commit revert。本 change 不依赖 cache layout / schema /
public API，revert 是 mechanical 的。

## Open Questions

- 是否需要在 `bench/` 的输出里加一行新的 metric 来跟踪 "NEON format
  helper inlining 是否生效"？比如 emit `static_assert` based on
  constexpr evaluation，或者直接 inspect 二进制 symbol 表确认 6 个函数
  不再 export？
  → 暂定**不加**：tasks.md 里直接用 `nm libgaby_vm.a | grep` 一次性手
    动验证；后续靠 throughput delta 反映效果。
