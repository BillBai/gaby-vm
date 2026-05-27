## Why

最近一次 cache 路径 profile (`docs/refs/gaby-vm-cache-hotpath-profile-
2026-05-27.md`) 显示 mixed workload 上 **~58%** 的执行时间花在 NEON
format helpers 加 `LogicVRegister::ClearForWrite` 这一连串小函数上，
其中 5 个 helper 单独占了 mixed 顶 of stack 的：

- `LaneSizeInBitsFromFormat(VectorFormat)` — 26.3% (2253 samples)
- `IsSVEFormat(VectorFormat)` — 17.7% (1517 samples)
- `LaneCountFromFormat(VectorFormat)` — 1.4% (118 samples)
- `LaneSizeInBytesFromFormat(VectorFormat)` — 0.2% (13 samples)
- `RegisterSizeInBytesFromFormat(VectorFormat)` — 0.3% (24 samples)

合计 **~45.9%** of mixed 在这 5 个函数本体上，**剩余 ~12% 在 `ClearForWrite`
里间接调它们**。

这 5 个函数本质都是 `VectorFormat`（6-bit 枚举）上的 lookup——switch
返回 lane size / lane count / format kind。它们的定义现在在
`src/aarch64/instructions-aarch64.cc`，是 externally-linkable 的函数。
每条 NEON / scalar FP 指令在 leaf 里会调它们 5-10 次，因为定义在 .cc
里，编译器没办法把它们 inline 进调用点，更没办法在 VectorFormat 是
constant 的情境下折成立即数。每次都付一次真实的 function call + switch
跳转 + 返回。

把它们搬进 header 并标 `constexpr inline`，编译器在调用点能直接做
constant folding——绝大多数调用是用 leaf 内部已知的 `VectorFormat`
literal 调的，可以折成立即数（lane count = 8 直接当 immediate 用）；
即便不是 constant 也能 inline 掉 function-call epilogue/prologue 那一对
开销。预期 mixed cache 路径从 ~39.55 ns/insn 砍到 ~25-28 ns/insn，
约 **1.4-1.6×**。smoke 路径不涉及 NEON，不受影响。

这是 cache 路径上的低垂果实：改动量极小（5 个函数 + 它们各自的
内部辅助常量 / 数组），语义零变化，VIXL marker 约定承担 boundary
管理。

## What Changes

- **把 5 个 NEON format helper 从 `.cc` 搬到 `.h`，标 `constexpr inline`**：
  - `vixl::aarch64::LaneSizeInBitsFromFormat(VectorFormat)`
  - `vixl::aarch64::IsSVEFormat(VectorFormat)`
  - `vixl::aarch64::LaneCountFromFormat(VectorFormat)`
  - `vixl::aarch64::LaneSizeInBytesFromFormat(VectorFormat)`
  - `vixl::aarch64::RegisterSizeInBytesFromFormat(VectorFormat)`

  签名、namespace 都保持不变；唯一变化是定义位置和 linkage。

- **Marker convention**：搬过来的函数体放进 `gaby-vm BEGIN` /
  `gaby-vm END` 标块（既在新 header 位置标，也在 `.cc` 的原位置标，
  说明"内容已上移至 header；这里刻意为空以保留 marker 上下文"）。
  这是 `docs/architecture.md` §VIXL Import Boundary 规定的约束。

- **重新跑 `bench/` 并把数据点录到 `docs/refs/`**：landing 之后跑 mixed +
  smoke 的 decoder/cache 各 7 次，写一份新 baseline-results-*.md 比
  current baseline (39.55/6.49 ns/insn) 看 mixed 的 delta；smoke 应该
  在 ±5% 噪声以内。

### Non-Goals

- 不动 `LogicVRegister::ClearForWrite` 本身（profile 显示它本体占
  12.1%，但要砍它得做 full-vector-write gating，那是单独的 lever B，
  本次不做）。
- 不动 NEON helper 的 heap allocation（lever C，单独做）。
- 不改 `ExecuteInstructionCached` 或 `PredecodedEntry`（dispatch 侧
  优化 lever D/E 独立排期）。
- 不引入新公共 API、不改 CMake target、不改 `include/gaby_vm/` 任何头
  文件。

## Capabilities

### New Capabilities

*(none — 纯实现层 perf 优化，不引入新 capability)*

### Modified Capabilities

- `aarch64-simulator`: 新增一条 requirement——把 6 个 NEON `VectorFormat`
  helper（`IsSVEFormat`、`LaneSizeInBitsFromFormat`、`LaneSizeInBytesFromFormat`、
  `LaneCountFromFormat`、`RegisterSizeInBitsFromFormat`、`RegisterSizeInBytesFromFormat`）
  定义为 `constexpr inline` 放在 `instructions-aarch64.h`，让编译器能在
  `VectorFormat` 是 compile-time constant 的调用点折成立即数。observable
  语义零变化（switch body byte-equivalent，`VIXL_ASSERT` / `VIXL_UNREACHABLE`
  行为保留），只新增"必须 constant-foldable"这条不变量，防止未来 maintenance
  把它们搬回 `.cc` 而不察觉。

## Impact

- **依赖**：依赖 `aarch64-simulator` 和 `predecode-cache` 这两个 capability
  的现有 implementation，但只用其调用 site，不改它们的 spec-level 行为。
- **Files**：
  - `src/aarch64/instructions-aarch64.h` (header 里加入 5 个 inline 函数体，
    marker block 包裹)
  - `src/aarch64/instructions-aarch64.cc` (移除 5 个函数体，marker block
    记录"definition lifted into header")
- **VIXL import boundary**：edits 全部在 marker block 内，**不修改任何
  upstream VIXL 函数签名、返回值、调用关系**。函数被 inline 之后 VIXL
  这边的所有 caller (Visit\* / \*Helper / `LogicVRegister::*`) 仍然能正常
  resolve。
- **Public API**：零变化。`include/gaby_vm/` 不动。embedder 不感知。
- **Binary**：所有调用点重新生成，整个 `libgaby_vm.a` 重 link。预期
  `bench_baseline` / `bench_smoke` 二进制略小（5 个函数体不再独立
  emit），但变化不显著。
- **Testing**：现有 `ctest` 必须全过 (`workload_shadow` 是 NEON 语义的
  最终 oracle)。bench number delta 录到 `docs/refs/` 作为新 baseline 数据点。
- **风险**：低。`constexpr` 让 5 个函数有了"constant-evaluable at call
  site if input is constant"的属性，但它们的内部逻辑（纯 switch）本来
  就是 constexpr-able 的。唯一可能踩雷的地方是 helper 当前 `.cc` 实现
  里如果有 static-local cache、宏展开依赖编译单元，或者依赖 `.cc` 里的
  internal-linkage helper——design.md 会逐函数检查并记录。
