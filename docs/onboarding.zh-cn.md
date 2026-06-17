# 代码上手指南

这份文档是给第一次（或久违地）翻这个仓库的人准备的一条主线：先建立全局认知，
再知道怎么编、怎么测、怎么 bench，以及每一块该去哪深挖。它不重复
`architecture.md` / `build.md` / `testing.md` / `bench/README.md` 里的细节，而是把
它们串起来——读完这篇，你应该知道**该读哪一篇、该看哪个文件**。

> 这是导读，不是权威。具体到某个规则有冲突时，以代码和那几篇专题文档为准。
> 几处专题文档目前和代码对不上（比如 C++ 标准），文末「已知文档漂移」一节列了出来。

---

## 0. 这个项目在做什么

gaby-vm 是一个**可嵌入的 AArch64 指令解释器**，语义直接复用 VIXL 的 AArch64
simulator。一句话概括它的优化思路：

```
预解码一次 -> 缓存解码后的分发目标 -> 重复执行缓存好的路径
```

注意几个它**不是**的东西，这决定了整套设计的边界：

- 不是 JIT。运行期不生成代码、不申请可执行内存。预解码出来的东西是**普通数据**。
  这正是它能干净嵌入 iOS 这类禁止运行期生成代码的环境的原因。
- 不是一个新 IR。缓存的是「这条指令该调哪个叶子函数」，不是把指令翻译成中间语言。
- 不是一个完整的系统模拟器。没有 MMU、没有设备模型、没有完整异常等级模型。V1
  只做 EL0/用户态 A64 执行。

目标平台首选 iOS（无 JIT），同时支持 macOS 和 POSIX 类环境（Linux / Android /
HarmonyOS）。Windows 暂不在范围内。

原则：**先正确，再快**。

---

## 1. 架构设计

### 1.1 双轨同源：理解这一点，其余都顺了

gaby-vm 有两条执行路径，跑的是**完全相同的指令语义**，区别只在「怎么取到下一条
该执行的叶子函数」：

| 轨道 | 公共入口 | 内部做法 | 用途 |
|------|----------|----------|------|
| **cache 轨** | `Simulator::RunFrom` / `StepOnce` | 预解码时缓存好每条指令的分发目标，稳态循环直接取缓存项调叶子 | 生产路径，性能优化的对象 |
| **decoder 轨** | `Simulator::DebugRunFrom` / `DebugStepOnce` | 沿用 VIXL 原始的 `Decoder → VisitNamedInstruction → leaf` 流程，每条指令现场解码 | 历史路径、诊断、差分测试的「对照组」 |

关键在于**两条轨最终调的是同一套叶子函数**（指令的真实语义实现）。机制是：两条轨
都通过同一个 `form_hash`（指令形态的哈希）查同一张 `FormToVisitorFnMap`，拿到同一个
指向 `Simulator` 成员函数的指针，再 `(this->*pmf)(pc)` 调下去。

- cache 轨的取项在 `Sources/gaby_vm/src/aarch64/simulator-aarch64.h` 的
  `ExecuteInstructionCached`（在 `// gaby-vm` marker 区域里）。
- decoder 轨在同文件的 `ExecuteInstruction`，叶子查表在
  `simulator-aarch64.cc` 里 `Hash(form) → map.find → (this->*fn)(instr)`。

这条「双轨同源」是整个项目能成立的支点：

- **优化只动重复执行的那条路**，不碰语义。修一个叶子的 bug，两条轨一起修好。
- **测试可以做差分**：同样的字节，cache 轨和 decoder 轨结果必须逐位相同（见第 3 节
  的 `vixl_port` 和 `ShadowRunner`）。这就是为什么这两条轨都得保留。

> 注意：两条循环**中途不互相切换**。一次 `RunFrom` 从头到尾走 cache 轨，一次
> `DebugRunFrom` 从头到尾走 decoder 轨。这样像 `form_hash_`、`last_instr_`、MOVPRFX
> 链这类执行中状态不会在切换瞬间错位。

### 1.2 PredecodeCache：缓存里到底存了什么

预解码缓存是 cache 轨的核心数据结构。

- 公共接口：`Sources/gaby_vm/include/gaby_vm/predecode_cache.h`
- 实现：`Sources/gaby_vm/src/gaby_vm/predecode_cache.cc`

用法上只有一个主入口：

```cpp
gaby_vm::PredecodeCache cache;
auto status = cache.RegisterCodeRange(code_ptr, size_bytes);  // 预解码一整段
// status == RegistrationStatus::Ok 才能跑
```

`RegisterCodeRange` 在注册时**一次性**把整段代码逐条解码，每条指令产出一个
`PredecodedEntry`（固定 16 字节，便于按 `(pc - start) / 4` 平坦索引）：

- `form_hash` —— 指令形态哈希，叶子查表用；
- `flags` —— 位标志（如 bit0 标记是否与 BTI 检查相关，让热路径多数指令跳过该检查）；
- `leaf` —— 不透明指针，指向那条指令对应的 `Simulator` 成员函数。

这一遍预解码的开销发生在注册时，**不在执行循环里**——所以稳态循环报的是缓存「执行」
速度，不含「构造」速度。运行时按 PC 找到所属 range（先查 Simulator 里缓存的当前
range，未命中再到 `cache.FindRange` 做二分查找），再按偏移索引到 entry，取 `leaf`
直接调。

### 1.3 代码地图：哪些是自己写的，哪些是 VIXL 导入的

这是个 **standalone 项目，不是 VIXL 的 in-place fork**。参考用的 VIXL 源码树在
`../vixl`（仓库外，只用来对照研究）。仓库里只导入了一个受控子集。

**gaby-vm 自己写的代码**——一共就三个 `.cc`（持「the gaby-vm authors」版权），加
六个公共头：

```
Sources/gaby_vm/include/gaby_vm/   <- 公共 API（gaby_vm:: 命名空间，不暴露任何 vixl:: 类型）
  gaby_vm.h            门面（主要转包 version）
  simulator.h          Simulator：双轨入口 + 强类型寄存器读写 + 钩子
  predecode_cache.h    PredecodeCache + PredecodedEntry / CodeRange / RegistrationStatus
  registers.h          强类型寄存器标识 + RegisterFile 快照
  shadow_runner.h      ShadowRunner 差分 oracle（测试用）
  version.h            版本号（header-only）

Sources/gaby_vm/src/gaby_vm/       <- 自己写的实现
  simulator.cc         Pimpl 包一层 vixl 的 Simulator，落地双轨 API、寄存器转接、钩子
  predecode_cache.cc   RegisterCodeRange 全流程、FindRange、预解码用的 visitor
  shadow_runner.cc     锁步跑两条轨、逐步比对寄存器与内存写、报第一个分歧
```

**从 VIXL 逐字节导入的代码**——其余都是，持 VIXL 的 BSD-3-Clause，布局镜像上游：

```
Sources/gaby_vm/src/               <- 共享根（utils-vixl、cpu-features、compiler-intrinsics…）
Sources/gaby_vm/src/aarch64/       <- AArch64 专属：simulator / decoder / instructions /
                                       logic / operands / registers / disasm / debugger …
```

导入范围按 `docs/refs/vixl-extraction-map.md` 的 **Tier 分级**，shipping 库只含
Tier 1–3。**Tier 0**（assembler / macro-assembler / code-buffer）**不进 shipping
库**——因为 gaby-vm 是指令字节的「消费者」，不生成代码。唯一的例外是测试用的那份
Tier-0 拷贝，见下。

**对导入文件的任何改动都要打 marker**，方便审计漂移：

- 单行：改动行上方加 `// gaby-vm:`，原因写在紧接的 `//` 注释里；
- 多行：`// gaby-vm BEGIN:` … `// gaby-vm END` 包起来。

一条命令枚举所有改动点：

```sh
git grep -nE 'gaby-vm( BEGIN| END|:)' Sources/gaby_vm/src/
```

cache 轨那一堆改动（`ExecuteInstructionCached`、`StepOnce`、分支钩子、可重入游标
存取等）就集中在 `simulator-aarch64.h` 的 marker 区里。想看「gaby-vm 在 VIXL 之上
加了什么」，从这条 grep 入手最快。

### 1.4 内存模型与线程模型（嵌入前必须知道）

**内存模型——直接打宿主地址空间**：guest 指针就是 host 指针，guest 的 load/store
就是对宿主堆/栈/全局的普通内存访问。没有 MMU、没有地址翻译、**没有边界检查**、没有
guest/host 之间的隔离层。越界访问要么直接把宿主进程 `SIGSEGV`，要么悄悄写坏宿主
状态。所以：

- 嵌入者负责把 guest 代码和数据放到 guest 期望的 host 地址上；
- guest 碰到的任何内存，其生命周期/对齐/别名都是嵌入者的责任；
- 一个行为不端的 guest，按构造就是一个宿主进程的 bug。

好处是解释器循环里没有边界检查（对 cache 模式的速度重要），而且天然契合 iOS 这种
「嵌入者本来就共享地址空间」的环境。

**线程模型——一个物理线程一个 `Simulator` 实例**，首次使用时惰性初始化。Simulator
内部不是为跨线程共享设计的，每实例状态（寄存器、系统寄存器、内部 scratch）归构造它
的那个线程所有。gaby-vm 不替你做线程本地的安置，嵌入者自己用 `thread_local` / TLS /
自己的线程上下文结构来放这个 per-thread 实例。

权威细节在 `docs/architecture.md`。

### 1.5 最小嵌入骨架

`demos/cli/main.cc` 是**官方的端到端嵌入范例**，强烈建议直接读它——它演示了完整
模式：手搓一段 guest 函数 → 注册到 cache → `RunFrom` 跑 → 用分支钩子拦截 guest 对
host 函数的调用并按 C ABI 转发。浓缩成骨架就是：

```cpp
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/simulator.h"
#include "gaby_vm/registers.h"

gaby_vm::PredecodeCache cache;
if (cache.RegisterCodeRange(code, size_bytes) !=
    gaby_vm::PredecodeCache::RegistrationStatus::Ok) { /* 处理失败 */ }

std::vector<uint8_t> stack(gaby_vm::Simulator::kMinStackSize);
gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

// 可选：拦截分支去调 host 函数 / 实现 FFI
sim.SetBranchHook(my_hook, &my_ctx);

// 可选：按 AArch64 C-ABI 喂参数
sim.Write(gaby_vm::GpRegister::X0, arg0);

sim.RunFrom(reinterpret_cast<uintptr_t>(code));   // cache 轨执行
uint64_t result = sim.Read(gaby_vm::GpRegister::X0);
```

把 `RunFrom` 换成 `DebugRunFrom` 就是 decoder 轨（不需要 cache 非空，可用 trace /
debugger），调试或对照时用。

跑一下这个 demo：

```sh
./build/debug/demos/cli/gaby-vm            # 跑嵌入 demo，打印 version + host_add 结果
./build/debug/demos/cli/gaby-vm --version
```

---

## 2. 如何编译

### 2.1 工具链要求

- **C++20**（不是 C++17——见文末漂移说明）。公共头用了 `std::span`、`std::variant`。
- CMake ≥ 3.21（presets 文件用 3.25 schema）。
- Ninja。
- 编译器：GCC / Clang / AppleClang。**没有 MSVC**，Windows 不在范围。

### 2.2 最快上手：用 presets

仓库带了 `dev-debug` 和 `dev-release` 两个 preset（都用 Ninja，都开
`compile_commands.json` 和 tests + demos）：

```sh
cmake --preset dev-debug          # 配置
cmake --build --preset dev-debug  # 编译
ctest --preset dev-debug          # 跑测试（只有 dev-debug 配了 test preset）
```

产物落在 `build/debug/`；release 同理落在 `build/release/`。release 的测试目前用
`ctest --test-dir build/release` 跑（还没配 release 的 test preset）。

### 2.3 编译选项

顶层 `CMakeLists.txt` 暴露这几个 option：

| 选项 | 默认 | 作用 |
|------|------|------|
| `GABY_VM_BUILD_TESTS` | 顶层构建时 ON | 编译 `test/` 下的测试 |
| `GABY_VM_BUILD_DEMOS` | 顶层构建时 ON | 编译 `demos/cli` |
| `GABY_VM_BUILD_BENCHMARKS` | 顶层构建时 ON | 编译 `bench/` 吞吐量 harness |
| `GABY_VM_BUILD_NATIVE_BASELINE` | **OFF** | 编译「相对原生」的对照工具，需同时开 BENCHMARKS，且 host 必须是 arm64 |

前三个的默认值是 `${PROJECT_IS_TOP_LEVEL}`：你独立构建这个仓库时自动是 ON；被别的
CMake 工程 `add_subdirectory` 进去时自动 OFF（消费者默认不会编你的测试和 demo）。

`GABY_VM_BUILD_NATIVE_BASELINE` 是开发专用、默认关：它会把 guest AArch64 字节放进
可执行内存直接在宿主 CPU 上跑（所以 host 必须 arm64），用来当「离原生有多远」的分母。
详见第 4 节。

### 2.4 构建结构里值得知道的两件事

不改 CMake 的话不用深究，但理解这两点能少踩坑（细节在 `docs/build.md`）：

- **警告策略是分裂的**。自己写的代码走严格策略（`-Wall -Wextra -Wpedantic`，
  `gaby_vm_apply_compile_flags`）；逐字节导入的 VIXL 源走放松策略（按文件加一串
  `-Wno-*`，`gaby_vm_apply_imported_compile_flags`），因为上游代码会触发 pedantic
  警告。两类源可以共存在同一个 `gaby_vm` target 里，靠的是文件级的属性设置。
- **VIXL 的编译宏是 `PRIVATE` 的**：`VIXL_INCLUDE_TARGET_A64`、
  `VIXL_INCLUDE_SIMULATOR_AARCH64`、Debug 下的 `VIXL_DEBUG`。它们不会通过
  `INTERFACE` 泄漏给消费者。需要直接碰 `vixl::aarch64::*` 头的测试/bench 要复刻这套
  「特权构建模式」（`PRIVATE` 引 `src/` + 同一套宏），见 `docs/build.md` 的
  *Privileged test build pattern*。

### 2.5 作为依赖被消费

CMake 消费者：

```cmake
add_subdirectory(third_party/gaby-vm)
target_link_libraries(your_target PRIVATE gaby_vm::gaby_vm)
```

也提供顶层 `Package.swift`（面向 C++ 的 SwiftPM 消费者，主力嵌入方用 SwiftPM 构建）。
注意：消费方**必须自己**设到 C++20（`cxxLanguageStandard: .cxx20`），因为 C++ 标准
不会跨包传递。CMake 仍是跑测试和 bench 的权威构建。两套构建编的是同一份源码。

---

## 3. 如何测试

### 3.1 测试体系长什么样

测试是 `test/` 下的一批独立可执行文件，每个一个翻译单元，链接
`gaby_vm::gaby_vm`，用 `add_test` 注册到 CTest。**目前没有引入 GoogleTest / Catch2**
这类框架——纯手写，够用为止（不是长期立场，将来要参数化 fixture 时引一个也行）。

跑全部：

```sh
ctest --preset dev-debug                 # = ctest --test-dir build/debug，失败时打印输出
ctest --test-dir build/debug -R <regex>  # 只跑名字匹配的
ctest --test-dir build/debug -N          # 只列出有哪些测试，不跑
```

### 3.2 实际注册了哪些测试

以 `test/CMakeLists.txt` 的 `add_test` 为准（`docs/testing.md` 的清单偏旧，少列了
一批）。按职责分大致是：

- **冒烟/构建契约**：`smoke`（公共 API）、`simulator_smoke`（单条 NOP 走 VIXL
  decode→visit→leaf）、`instructions_aarch64_constexpr_smoke`（一组 `static_assert`，
  编过即通过）。
- **正确性**：`simulator_correctness`（手编码序列同时过 cache 轨 `RunFrom` 和 decoder
  轨 `DebugRunFrom`，比对到预期状态）。
- **公共 API 行为**：`typed_register_io` / `typed_register_io_abort`（强类型寄存器读
  写，含越界 fail-fast）、`simulator_constructor_stack`（栈大小契约，过小必须 abort）、
  `branch_hook_dispatch` / `branch_hook_reentrancy`（分支钩子分发与可重入）、
  `reentrancy`（可重入 + 双轨终止）。
- **差分 oracle**：`shadow_runner`（ShadowRunner 自检：匹配时零分歧 + 故意注入缺陷时
  能抓到分歧）、`workload_shadow`（用 ShadowRunner 跑所有已提交的 bench workload，断言
  两轨零分歧）。
- **移植的 VIXL 大套件**：`vixl_port_integer` / `vixl_port_fp` / `vixl_port_neon`
  是核心三个，单独讲（下一节）。同一个孤岛还附带几个基建自检：`vixl_asm_island_smoke`、
  `vixl_asm_harness_smoke`、以及 `vixl_asm_harness_selftest_{cache,decoder,reference,baseline}`
  四个场景（验证崩溃护栏在不同执行阶段的行为）。它们每次全量 `ctest` 都会跑，但守门
  命令 `-R vixl_port` 只匹配那三个核心套件。

### 3.3 `vixl_port`：动热路径前后都要跑的护栏

这是覆盖面最广的正确性护栏，专门为了**抓住预解码/分发优化引入的回归**而建。它从
VIXL 自己的执行测试套件移植而来，**现场汇编（live-assemble）**每个上游 `TEST()` 函数
体，然后在两条 gaby 轨上各跑一遍。

它怎么工作（细节在 `docs/testing.md` 的 *Ported VIXL tests*）：

- 有一份**测试专用的 Tier-0 汇编器孤岛**在 `test/test_support/vixl_asm/`——VIXL 的
  assembler + macro-assembler + code-buffer + 测试基建的逐字节拷贝，钉在
  `vixl-extraction-map.md` 记录的导入 SHA 上。它编成 `gaby_vm_vixl_asm_testonly`，
  `PRIVATE`-link `gaby_vm::gaby_vm`，**永不**进 shipping 库（无 `::` 别名、带
  `_testonly` 后缀、`GABY_VM_BUILD_TESTS` 门控、只编 `VIXL_CODE_BUFFER_MALLOC`）。
  汇编出来的字节是普通 `malloc` 数据，喂给 gaby 解码器，**不在宿主 CPU 上执行**——
  no-JIT / no-RWX / iOS 约束照样成立。
- 因为是现场汇编，`Mov(reg, 真实地址)` 会烤进一个进程内真实地址，所以
  **load/store/ADR/literal、乃至原子/独占/CAS 这类读改写指令都是对真实内存真跑的**。
- 每个函数体过**两道 oracle**（都覆盖寄存器和内存）：
  - **差分**：两条 gaby 轨必须一致——抓 cache 轨分发回归的主力；
  - **绝对**：gaby 必须和一个 VIXL 参考 simulator 的结果一致——抓「两条轨一起错」。

**什么时候必须跑**：动了共享执行热路径（解码/分发、预解码缓存、或导入的 VIXL 叶子
语义）**之前和之后**，都要跑且保持全绿：

```sh
ctest --test-dir build/debug -R vixl_port
```

它自包含，**不需要** `../vixl`。别在红的或没跑过的套件上落性能改动。

几个会让你困惑的点先说在前面：

- **它不能在 ASan 下编**。内存 oracle 是对一个活的 C++ 栈帧做 `memcpy`，会和 ASan 的
  栈红区打架。这里的内存安全检查靠的是双轨 + 参考三方差分，不是 sanitizer。
- **有 baseline 计数护栏**：套件会钉住每个 family「跑了多少 / 跳过多少」的预期split，
  漂了就 FAIL——防止某个回归把用例从「跑」悄悄挪到「跳过」从而隐形缩水覆盖面。有意
  改动后用 `VIXL_PORT_REBASELINE=1` 重新基线并在同一个 commit 更新表。
- 有一批用例会被**有意跳过**（能力不支持、不确定性、data-in-stream 等），`VIXL_PORT_TRACE=1`
  能打印每个用例被跳的原因。
- **升级 VIXL 后**没有 fixture 要重生成：重新在新 SHA 拷一遍孤岛、更新
  `vixl-extraction-map.md` 里的 SHA、重跑即可。

### 3.4 加一个新测试

- 在 `test/CMakeLists.txt` 加一对 `add_executable` + `add_test`；
- 用 `gaby_vm_apply_compile_flags`（项目策略；导入源那套放松 helper 不适合测试）；
- 只碰公共 API 的测试别用特权构建（保持默认 API 表面诚实）；要碰 `vixl::aarch64::*`
  才用特权构建模式（`PRIVATE` 引 `src/` + VIXL 宏）；
- 失败时退非零，stdout 要点名失败子用例并打印 实际 vs 预期。

> 编码策略：shipping 树里没有汇编器，所以 `simulator_correctness` 这类测试的指令是
> 裸 `uint32_t` 数组。需要新编码时，授权阶段用外部工具（如 `llvm-mc`）出 hex 再手抄
> 进源码——构建和运行期都不依赖汇编器机器。

---

## 4. 如何 bench

### 4.1 先分清：ctest 管正确性，bench 管性能

`bench/` 是**开发者手动调用**的吞吐量 harness，**不注册到 CTest**（ctest 只跑正确性）。
直接从构建目录运行二进制。它测的是一段固定指令负载在某种执行模式下的吞吐。

权威说明在 `bench/README.md`。

### 4.2 有哪些二进制

| 二进制 | 负载 | 用途 |
|--------|------|------|
| `bench_baseline` | upstream-VIXL 生成的 mixed 负载（~64k 动态指令/次，68% NEON） | 合成压力测试，**不**代表业务逻辑 |
| `bench_smoke` | 32 条直线 ALU（`llvm-mc` 汇编） | 毫秒级，验证 harness 管线本身 |
| `bench_business` | 5 个编译出来的 C 微内核 | **代表性**的「iOS 业务逻辑慢多少」 |
| `native_baseline` / `native_smoke` / `native_business` | 同样的字节，直接在宿主 arm64 CPU 上跑 | 「离原生有多远」的诚实分母（需 `GABY_VM_BUILD_NATIVE_BASELINE=ON`） |

`bench_business` 的五个微内核：`parse`（变长记录解析）、`hash`（FNV-1a，整数依赖
链、分支少，最好情况）、`struct`（结构体数组变换）、`fsm`（状态机扫描，逐字节不可
预测分发，最坏情况）——前四个纯标量整数；`applogic` 是**唯一带 FP/NEON 的**（~9% 标量
double，对应 CGFloat 几何），是最贴近真实 iOS 业务逻辑的那个。

### 4.3 构建与运行

bench 需要 release（性能数才有意义），顶层构建时 `GABY_VM_BUILD_BENCHMARKS` 已默认 ON：

```sh
cmake --preset dev-release
cmake --build --preset dev-release
```

要原生分母，额外开（host 必须 arm64）：

```sh
cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON -DGABY_VM_BUILD_NATIVE_BASELINE=ON
cmake --build --preset dev-release
```

跑（核心 flag 是 `--mode {decoder|cache}`，两种模式数字直接可比）：

```sh
./build/release/bench/bench_business --mode cache   --seconds 2.0   # 全部内核，cache 轨
./build/release/bench/bench_business --mode decoder --seconds 1.0   # decoder 轨
./build/release/bench/bench_business --kernel hash  --mode cache    # 只跑一个内核
./build/release/bench/native_business               --seconds 1.0   # 宿主 arm64 分母
./build/release/bench/bench_business --verify                       # 正确性闸（见下）
./build/release/bench/bench_baseline --help                         # flag 与默认值
```

每次跑打印一组 `key: value`，主指标是 `iterations_per_second`（每秒跑完整负载多少遍），
这是跨 native / decoder / cache 都良定义、可直接比的量。

### 4.4 该看哪个数、什么时候必须跑

- **代表性「慢多少」**：引用 `bench_business` 的 cache 轨 对 `native_business`，按内核
  分形报告。已测基线大致是：四个标量内核 cache 轨 ~6.5 ns/insn（所以它们的 slowdown
  从 ≈19×(hash) 到 ≈210×(struct)，主要由原生侧 IPC 决定，不是 gaby 的锅）；带 FP/NEON
  的 `applogic` 打破这条平线到 ~10 ns/insn，是最差的 ≈330×——VIXL 的 FP/NEON 叶子比
  整数叶子更贵。
- **改了叶子或内核之后**：先跑 `bench_business --verify`——它单步两条轨、数动态指令、
  交叉校验 cache==decoder 以及 x0 对已提交期望值。这是性能改动的正确性闸。
- **改动以执行速度为目标时**：用这套 harness 给出 before/after，别靠眼估。

### 4.5 方法论提醒

V1 这套 harness 追求的是**数量级正确**（「cache 帮了 3× 吗」），不是发表级精度。典型
非专用机器上跑到跑之间有几十个百分点的方差——多跑几遍、看数量级、别盯尾数。尽量在
基本空闲、别用电池的机器上跑。

> 记忆里有一条相关偏好：bench 精度到数量级就够，不必搞复杂的噪声控制协议。

---

## 5. 接下来去哪深挖

| 你想了解 | 读这个 |
|----------|--------|
| 架构、内存/线程模型、VIXL 导入边界、marker 约定 | `docs/architecture.md` |
| 内部构建结构（target、警告策略、VIXL 宏作用域） | `docs/build.md` |
| 编码规范（格式、命名空间、license 头、marker） | `docs/conventions.md` |
| 测试策略、`vixl_port` 护栏、编码策略 | `docs/testing.md` |
| 性能测量方法、业务微内核 | `bench/README.md` |
| VIXL 导入 Tier 清单 | `docs/refs/vixl-extraction-map.md` |
| 规范性能力需求 | `openspec/specs/` |
| 用户向构建/嵌入说明 | `README.md` |
| 项目目标与 Agent 工作守则 | `AGENTS.md`（= `CLAUDE.md`） |
| 设计/调研记录（按日期） | `docs/refs/` |

读代码的几条快捷路径：

- 「gaby-vm 在 VIXL 之上改了/加了什么」→ `git grep -nE 'gaby-vm( BEGIN| END|:)' Sources/gaby_vm/src/`
- 「cache 轨热路径」→ `Sources/gaby_vm/src/aarch64/simulator-aarch64.h` 的 `ExecuteInstructionCached`
- 「公共 API 全貌」→ `Sources/gaby_vm/include/gaby_vm/`
- 「一个完整嵌入例子」→ `demos/cli/main.cc`

---

## 6. 已知文档漂移（写作本文时）

几处专题文档落后于代码，按代码为准：

- **C++ 标准**：顶层 `CMakeLists.txt` 是 `set(CMAKE_CXX_STANDARD 20)`，库 target 是
  `cxx_std_20`；但 `docs/build.md` 仍写 C++17，`README.md` 顶部也写「C++17 compiler」
  （而它自己的 SwiftPM 一节又要求 C++20）。**实际是 C++20。**
- **bench 二进制清单**：`docs/build.md` 只提了 `bench_baseline` / `bench_smoke`，漏了
  `bench_business`（以及 native_* 系列）。
- **测试目标清单**：`docs/testing.md` 的 *Current targets* 只列了一部分，实际
  `test/CMakeLists.txt` 注册的测试见本文 3.2。
