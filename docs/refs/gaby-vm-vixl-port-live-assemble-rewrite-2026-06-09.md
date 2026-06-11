# vixl_port 推倒重来：从「冻结 fixture 回放」改成「现场 assemble + 两轨跑」— 设计稿 — 2026-06-09

> **它是什么**：一份**已和负责人对齐**的设计稿，记录把 `vixl_port` 测试套件从
> 「编写期提取字节、提交 fixture、运行期回放」整套**推倒重来**，改成「测试时把上游
> VIXL `TEST()` 体**现场 assemble**，在 gaby 两条执行轨上跑」。它取代前一版
> [`gaby-vm-vixl-sim-test-port-design-2026-06-08.md`](./gaby-vm-vixl-sim-test-port-design-2026-06-08.md)
> 选定的「冻结 fixture」路线——那条路线本身能跑、覆盖也广，但有一个结构性死角，下面
> 第 1 节说清楚。
>
> **它不是什么**：不是排期表，不是 OpenSpec change。源码引用用 `path:line`，路径相对
> 仓库根；`../vixl` 指参考 VIXL 树。
>
> **状态**：方案已对齐（2026-06-09），SHA 已确认，待按第 6 节分阶段实现。源头讨论见本
> 文末「索引」。

## TL;DR

- 现有冻结 fixture 模型有个治不好的死角：**它表达不了访存语义**。upstream 每个带
  load/store 的 `TEST()` 体都被丢掉了（integer 264 个里丢了约 174 个），根因是测试体把
  缓冲区的**宿主地址**硬编码进指令，换到 gaby 进程里回放就指向垃圾内存、一访问就 fault。
  整套 `LDR/STR/LDP/STP/原子/独占/CAS` 一条都没覆盖。
- 解法不是给冻结模型打补丁（那需要一套很重的「NOP 掉 Mov + 记录绑定 + 内存 oracle」机器），
  而是**换掉地基**：让测试 binary 链接 VIXL 的 assembler，**现场把测试体汇编出来**。这样
  `Mov(reg, reinterpret_cast<uintptr_t>(真实 scratch buffer))` 是对着进程内真地址现编的，
  baked 地址问题当场消失，load/store / ADR/ADRP / literal 全部能跑，覆盖冲向 EL0 接近全集。
- 关键边界：**no-JIT / no-RWX 一条不破**。汇编产物只是一块普通 `malloc` 缓冲区（用
  `VIXL_CODE_BUFFER_MALLOC` 强制），gaby 把这些字节喂解码器，从不在原地原生执行。assemble
  指令 ≠ JIT，这跟「编写期用 llvm-mc 出 hex」是同一性质——只是把工具换成 VIXL 自己的汇编器、
  且搬到测试运行期。
- 汇编器**只进测试、绝不进 `gaby_vm` 出货库**：拷进一个测试专用目录 `test/test_support/vixl_asm/`，
  CMake 上做硬隔离，文件头标清楚。

## 1. 为什么推倒重来

前一版选了「冻结 fixture」：编写期用真 VIXL 把每个 `TEST()` 体汇编成字节 + 采集期望状态，
提交进仓库，运行期 gaby 只回放这些字节。好处是 `ctest` 完全自包含（不需要 `../vixl`、不编
汇编器）。代价是**回放的是死字节**——而 VIXL 的访存测试长这样：

```cpp
uint64_t src[2] = {...};                              // 宿主上开缓冲区
__ Mov(x17, reinterpret_cast<uintptr_t>(src));        // 把 src 的真实地址塞进 x17
__ Ldr(w0, MemOperand(x17));                          // 从 src 读
ASSERT_EQUAL_64(0x76543210, x0);
```

那行 `Mov` 把**抽取那台机器、那个进程**里 `src` 的真实地址硬编码进了指令。冻结下来、换到
gaby 进程回放，这个地址失效 → fault。所以提取工具的结构性过滤器（`capture_macros.h` 的
`IsNonPortableInstr`）只能把**任何含 load/store / ADR/ADRP / 寄存器间接跳转的体整段丢掉**。
后果在 `docs/testing.md` 里已经白纸黑字写了：**ported 套件不覆盖任何内存访问语义**。

这个死角不是测试没写全，是**模型本身表达不了**。而 dispatch / 操作数预解码那套优化恰恰会
碰到访存执行路径——guard rail 在这块是瞎的，等于没兜住最该兜的地方。

给冻结模型补这个洞需要很重的机器（运行期分配缓冲区、把基址播种进寄存器、抹掉 baked 地址、
加内存 oracle、每轨复位……评估过一版，复杂且只能覆盖一部分）。换地基反而更干净：**现场
assemble**，地址在运行期对着真缓冲区现取，洞从根上不存在。

## 2. 关键地基事实（都已核实）

1. **VIXL 模拟器是「同地址空间」内存模型**：客户机虚拟地址**就是**宿主指针。
   `simulator-aarch64.h:410-449` 的 `Memory::Read/Write` 直接 `reinterpret_cast<char*>(AddressUntag(addr))`
   + `memcpy`，没有软 MMU、没有地址翻译。栈也是这么做的——`ResetState` 里
   `WriteSp(memory_.GetStack().GetBase())`（`simulator-aarch64.cc:770`），SP 就是一块宿主堆
   缓冲区的真实地址。所以测试现场 assemble 出 `Mov(reg, 真地址)`，三个引擎（VIXL 参考 sim、
   gaby cache 轨、gaby decoder 轨）在同进程看到的是**同一个真地址**，访存自然一致。

2. **gaby 当初只导入了 simulator/decoder 链，没导入 assembler 链**。所以「缺口」是一组界限
   清楚的文件（见第 3.2 节 A 组），而 MacroAssembler 需要的 8 个底层共享头
   （`instructions / operands / registers / constants / cpu-features / utils / globals /
   compiler-intrinsics`）**已经编进 `gaby_vm` 库了**。

3. **`gaby_vm` 出货库零链接依赖**（`Sources/gaby_vm/src/CMakeLists.txt` 没有任何
   `target_link_libraries`）。这是「测试专用库绝不会被拉回出货库」的硬保证——没有任何路径
   能让 island 渗回 `gaby_vm.a`。

4. **导入 SHA 已确认 = `../vixl` HEAD `160c445`（2026-05-14）**。证据：gaby 没改过的
   `registers-aarch64.h` / `constants-aarch64.h` 跟 HEAD 逐字节一致；`instructions-aarch64.h`
   那 271 行差异是 gaby 自己标了 `gaby-vm BEGIN` marker 的 constexpr-inline 性能改动，不是版本
   错位。所以从 HEAD 拷 Tier-0 安全。**拷之前 `git -C ../vixl checkout 160c445`**，并把这个 SHA
   记进 `vixl-extraction-map.md`——这是消除「拷来的 .cc 和 gaby 手改过的头之间 silent ODR」的
   关键。

## 3. 方案：一个测试专用的 assembler「孤岛」

### 3.1 放哪、怎么标

目录：**`test/test_support/vixl_asm/`**（归属 `test/`，天然在出货编译范围之外）。

每个拷进来的 VIXL 文件：**保留原版权头逐字不动**，下面再加这段 marker（大白话，不用「出货」
这种黑话）：

```
// TEST-ONLY — NOT part of the gaby_vm library. Do not include or link this from
// gaby_vm or anything that embeds it.
//
// 仅供测试：本文件是 VIXL AArch64 汇编器的拷贝，只用于构建 vixl_port 测试（测试时
// 把指令序列汇编成机器码）。它不属于 gaby_vm 库，不会被编进对外发布的产物。gaby_vm
// 库本身、以及任何把 gaby_vm 嵌进去的代码，都不要 include 或链接本目录里的任何文件
// ——它被刻意挡在 gaby_vm 编译目标之外，只有测试才链接它。
```

孤岛根目录再放一个 `.clang-format`（`DisableFormat: true`），免得 format-on-save 把拷进来的
上游文件重排、破坏和上游的可 diff 性。

### 3.2 拷哪些文件（就这些，从钉死的 SHA 拷）

**A 组 · assembler 本体**（核心 delta，gaby 没有的）：`assembler-aarch64.{h,cc}`、
`macro-assembler-aarch64.{h,cc}`、SVE 两个 `.cc`（`assembler-sve-aarch64.cc`、
`macro-assembler-sve-aarch64.cc`——只为补足非 SVE 头里声明的 out-of-line 成员符号，不跑 SVE
测试）、`assembler-base-vixl.h`、`code-buffer-vixl.{h,cc}`、`code-generation-scopes-vixl.h`、
`macro-assembler-interface.h`、`invalset-vixl.h`。

**T 组 · VIXL 测试基础设施**：`test-utils-aarch64.{h,cc}`（提供每个 `TEST` 体都引用的
`RegisterDump core`）、`test-simulator-inputs-aarch64.h`（76KB FP/NEON 输入表，
`test-utils-aarch64.cc:34` 要它，少了编不过）、`test-utils.h`、`test-runner.{h,cc}`
（`vixl::Test` 链表 + 注册宏；它的 `main()` 在 CMake 里改名让位给我们的 harness）。

**U 组 · 上游测试体**（原样拷，拷时去掉那行无 include guard 的自 include）：
`test-assembler-aarch64.cc` / `-fp-aarch64.cc` / `-neon-aarch64.cc`。

**S 组 · gaby 自己写、不从 VIXL 拷**：`test-utils-stub.cc`（见第 5 节 no-RWX 红线）。

### 3.3 怎么保证 `gaby_vm` 碰不到它

- 测试库取名 `gaby_vm_vixl_asm_testonly`，**故意不给 `::` 别名**、名字带 `_testonly`，一 grep
  就知道不能出货；只在 `GABY_VM_BUILD_TESTS` 下、从 `test/` 进去才 `add_subdirectory`。
- `Sources/gaby_vm/src/CMakeLists.txt` **一字不改**，A 组任何文件绝不进 `GABY_VM_IMPORTED_SOURCES`。
  靠第 2.3 条那个「零链接依赖」事实，island 渗不回 `gaby_vm.a`。
- 测试 exe 用 **PRIVATE** 链 `gaby_vm_vixl_asm_testonly`，也漏不出去。

### 3.4 ODR / 重复符号怎么避免（这是最该小心的地方）

MacroAssembler 和 Simulator 共享那 8 个底层头，它们的符号已经编进 `gaby_vm`。所以：

- **绝不把这 8 个共享头/源再拷一份**进孤岛——拷了就是同一个 `vixl::` 符号定义两遍，链接撞车。
- 孤岛的 `target_include_directories` 把 **`Sources/gaby_vm/src` 排在第一**：拷进来的 assembler
  里 `#include "operands-aarch64.h"` / `#include "simulator-aarch64.h"` 等全部解析到**已导入的
  那份**（包括 `macro-assembler-aarch64.h:41` 那个无条件 include 的 simulator——它指到 gaby
  改过的双轨 simulator，正好是我们要的）。
- 孤岛库 **PRIVATE 链 `gaby_vm::gaby_vm`**，共享 leaf 符号全从 `gaby_vm.a` 来，只定义一次。
- 第 2.4 条钉 SHA 就是为这条服务：拷来的 .cc 必须和 gaby 手改过的头来自同一 SHA，否则可能撞上
  无诊断的 inline-body ODR。

## 4. 跑测试 + oracle

复用提取工具已经验证过的那招（`tools/vixl_test_extract/capture_macros.h` 是模板）：**重定义
`SETUP/START/END/RUN/ASSERT_EQUAL_*` 宏，再原样 `#include` 上游测试 .cc**，每个 `TEST()` 体逐字
编译。区别只在 `RUN()` 不再「采集冻结」，而是**现场在 gaby 两条轨上跑**。

每个用例一个 rig，持有：一个真 `MacroAssembler`（写进 `VIXL_CODE_BUFFER_MALLOC` 普通堆缓冲区）；
一个 VIXL 参考 `Simulator`（**只用来算绝对期望值**，即 `core.Dump`，跟提取工具一样）；两个
gaby `Simulator`（cache 轨带 `PredecodeCache`、decoder 轨不带），共享一个 `PredecodeCache` 和一块
栈；外加一块普通堆 scratch buffer 给测试体当 load/store 落点。

- `START()/END()` 跟提取工具一致：`END()` 里 `EmitLiteralPool(kBranchRequired)` 把字面量池强制
  inline（LDR-literal 跟着体走）、`core.Dump(&masm)` 出绝对锚、`Br(xzr)` 收尾（不依赖 LR）。
- **绝对 oracle**：每个 `ASSERT_EQUAL_*` 拿 VIXL 参考 sim 算出的 `core` 期望值，对**两条 gaby 轨**
  跑完的寄存器/标志位逐项查（移植 `vixl_port_runner.cc:40-99` 的 `CheckAssert`）。
- **差分 oracle**：cache 轨 == decoder 轨，整个 `RegisterFile` 对拍（移植
  `vixl_port_runner.cc:138-172` 的 `DifferentialEqual`）。这条是给 dispatch 优化兜回归的主力。
- **内存白来**：体里 `Mov(reg, reinterpret_cast<uintptr_t>(rig.scratch))`，三引擎看同一真地址，
  load/store/ADR/literal 全部真跑；`ASSERT_EQUAL_MEMORY` 从「强制跳过」变成真检查。结构性跳过类
  整个塌掉。
- **入口状态显式播种**：两条 gaby 轨跑前显式 reset 到一个**和 VIXL `Simulator::ResetState()` 逐字段
  一致**的状态（启动时跑一次等价性断言守住），不靠「让体自己建状态」。gaby 的 `ResetState()` 在
  `simulator-aarch64.cc:764`，两轨 `RunFrom` 前都调。
- 每个 family 一个 `main()`：装上提取工具那套 `sigsetjmp + alarm` 崩溃/挂死守护
  （`extract_main.cc:66-113`，病态体跳过而非崩套件），走 `vixl::Test` 链表逐个跑。

**保留的跳过面**（是 gaby 真实能力缺口，不是结构性跳过）：特性 deny-list
（MTE/BF16/PAuth/HBC/…）和按名隔离（printf/runtime_calls/dc_zva/system/mops/…）。**丢掉**的是
结构性过滤器 `IsNonPortableInstr`——load/store/PC-rel 现在能跑了。运行期只留安全网：崩溃/挂死
守护 + 每用例指令上限（兜住跑飞的循环，作为跳过上报）。

## 5. no-RWX 红线（审查抓出来的，必须守住）

VIXL 的 `test/test-utils.cc` 里有一行无条件 `#include <sys/mman.h>` + `ExecuteMemory()`——它
mmap 出可执行内存、**原生 call** 汇编出来的字节。这是 no-JIT/no-RWX/iOS 直接违规。处理：

- **坚决不拷 `test-utils.cc`**。`ExecuteMemory` 在 simulator 路径上哪都没被引用（已 grep 核实）。
- 写一个 gaby 自己的 `test-utils-stub.cc`（S 组），把 `test-utils.h` 里测试基础设施真正会链到的
  那几个符号补上，其中 `ExecuteMemory()` 做成 no-op / abort-on-call。确切符号集靠链接时报的
  undefined symbol 逐个补（预期就 `ExecuteMemory` 一个）。
- `code-buffer-vixl.cc` 里 mmap/mprotect 全在 `VIXL_CODE_BUFFER_MMAP` 后面；用
  **`-DVIXL_CODE_BUFFER_MALLOC`** 编，它就是纯 malloc/free，`SetExecutable()/SetWritable()` 变
  no-op。这个 define 要 PUBLIC 给到每个碰 code-buffer 的 TU。

结论：整个孤岛**不含 `sys/mman`、不原生执行任何汇编字节**，no-JIT/no-RWX/iOS 那条线一字不破。

## 6. 迁移顺序（全程保持绿，工具最后才删）

1. **钉 SHA**：`git -C ../vixl checkout 160c445`，把 SHA 记进 `vixl-extraction-map.md`。
2. **落孤岛，只编不接**：拷 A/T/U 组、写 S 组 stub、剥掉自 include、盖 marker、加
   `.clang-format`、写 `gaby_vm_vixl_asm_testonly` 的 CMake。**现有冻结套件一字不动、照样绿。**
3. **ODR/链接 smoke 闸**（动 harness 之前的硬关卡）：把孤岛 + `gaby_vm.a` 链成一个 binary，跑一条
   `Mov(x0,1) + FinalizeCode`，再**用 VIXL 参考 sim 真跑一个体**（证明
   `macro-assembler-aarch64.h:41` → gaby simulator 这条耦合能编能链）。`nm` 确认零重复 `vixl::`
   符号、零未定义 SVE 符号、不链 `sys/mman`。
4. 写 `gaby_two_track_macros.h`（两轨宏 + 入口播种）和 `vixl_port_oracle.{h,cc}`（移植 runner 的
   `SeatEntry/CheckAssert/DifferentialEqual` + ResetState 等价断言）。先单独编过。
5. **先上 integer 一个 family**，临时名 `vixl_port_integer_live`，和旧的并存。调绿，确认纳入用例数
   明显超过旧的 90 条（覆盖增益）。
6. 切换 integer：删它的 `.inc`，`_live` 改回正名。fp、neon 各重复一遍，每步 `ctest -R vixl_port` 保持绿。
7. 三族都活了：删 `vixl_port_fixture.h`、`vixl_port_runner.{h,cc}`、整个 `generated/`、manifest。
8. **最后**才删提取工具：移除 `tools/vixl_test_extract/` + 顶层 CMake 的 `GABY_VM_BUILD_VIXL_EXTRACT`
   option 和 `add_subdirectory`。Debug/Release 全预设干净构建 + 全 ctest 确认没人引用它。
   （删之前把 `capture_macros.h` 的 duck-typed 断言记录器和崩溃守护抢救进新 harness。）
9. 改文档：`vixl-extraction-map.md`（Tier-0 已拷进 test-only 孤岛、钉 SHA、排除出 `gaby_vm`）、
   `architecture.md`（import 边界区分「出货导入 Tier 1-3」和「测试专用 Tier-0 拷贝」）、
   `docs/testing.md`（vixl_port 现在是现场汇编两轨跑、无冻结 fixture、无再生工具）、
   `AGENTS.md`（删掉「behind `GABY_VM_BUILD_VIXL_EXTRACT` 再生 fixture」那句）。upstream 版权头全程不动。

## 7. 风险

- **SVE 符号完整性**：非 SVE 的 assembler/macro 头里声明了大量在 SVE .cc 里定义的 out-of-line 成员，
  少拷就是 undefined symbol。缓解：第 3 步 `nm`/链接 smoke 立刻暴露；孤岛源文件集照提取工具
  `_vixl_srcs` 原样镜像。
- **stub 符号集**：`test-utils-stub.cc` 必须正好覆盖 `test-utils-aarch64.*` 链到的那几个符号；多了
  少了都在链接期暴露，迭代补齐，**永远不碰 mmap 那条真实现**。
- **头版本错位（silent ODR）**：拷来的 .cc 是对着上游头写的，却编译在 gaby 手改过的头上
  （constexpr-inline 的 VectorFormat helper 等）。同 SHA 下是自洽的；**不钉 SHA 就可能撞无诊断的
  inline-body ODR**。缓解：第 1 步钉 SHA，extraction-map 记 SHA，将来升级两半一起 re-sync。
- **入口状态分歧**：gaby 默认构造态若和 VIXL `ResetState()` 不一致，oracle 可能对某个「读了但没写」
  的寄存器误判。缓解：两轨显式同样播种 + 一次性逐字段等价断言，**以播种为准、不以构造默认为准**。
- **现场访存/跳转 fault**：体里 `Mov(reg, 宿主地址)` + 乱写可能踩坏测试进程；BR/BLR 跳到非 rig 指针
  会落到野 PC。缓解：保留按名隔离 + 崩溃/挂死守护 + 每用例指令上限；只把 rig 自己的、已登记进 cache
  的 scratch buffer 暴露给体。
- **过早删工具**：孤岛 + `gaby_vm.a` 单 binary 链接从没构建过；唯一验证过的 assembler+sim 自包含
  构建就是提取工具。缓解：工具默认 OFF 留到三族全绿，删在最后一步（可逆）。
- **覆盖不再冻结**：现场体若新踩到 gaby 未实现的 leaf，会让套件 FAIL 而非被预先跳过。这其实是好事
  （暴露真缺口），但改变了套件性格。缓解：特性 deny-list + 按名隔离是显式可审的跳过面，新 fail 当
  triage 项处理。

## 8. 已定的取舍 / 仍待确认

已定（负责人 2026-06-09 拍板）：

- 目录 = **`test/test_support/vixl_asm/`**（不放 `Sources/`）。
- marker 用第 3.1 节那段大白话（不用「出货代码」这种黑话）。
- **拷贝**而非链接 `../vixl`——保住「不需要 `../vixl` 也能编/跑」这个现有属性。
- SVE 只跳过不跑（gaby 无 SVE leaf）；SVE assembler .cc 只为补链接符号。
- 提取工具留到最后删，不永久保留。

仍可在实现期微调：`test-utils-stub.cc` 的确切符号集（靠链接暴露）；某 family 若太慢再按 CTest 进一步切分。

## 9. 索引

- 前一版（被本文取代的冻结 fixture 路线）：
  [`gaby-vm-vixl-sim-test-port-design-2026-06-08.md`](./gaby-vm-vixl-sim-test-port-design-2026-06-08.md)
  及其 `-plan-`；落地后 `docs/testing.md` 的「Ported VIXL tests (`vixl_port`)」一节也要按第 6.9 步改。
- 触发本次推倒重来的讨论：访存覆盖死角（`docs/testing.md` 里「ported 套件不覆盖任何内存访问语义」
  那段）+ Apple Silicon 无通用 SVE（向量化落 NEON，SVE 测试整族不需要）。
- 受影响的边界文档：[`vixl-extraction-map.md`](./vixl-extraction-map.md)（Tier-0 现在 test-only 拷贝）、
  [`../architecture.md`](../architecture.md)（VIXL import 边界）、[`../testing.md`](../testing.md)、
  [`../../AGENTS.md`](../../AGENTS.md) 的 guard rail 条目。
