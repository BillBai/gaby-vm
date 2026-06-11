# VIXL 模拟器测试移植 — 实现计划 — 2026-06-08

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 VIXL AArch64 执行类测试（test-assembler-aarch64/-fp/-neon）成规模搬进 gaby-vm，作为激进性能优化前的正确性 guard rail。

**Architecture:** 两段分离。编写期（authorship-time）一个链接 `../vixl` 真 MacroAssembler+Simulator 的提取工具，用「宏重定义」吞下 VIXL 测试体，吐出 `{body 字节, body 入口寄存器状态, ASSERT 目标值, 需要的特性}` 的 fixture，并把生成产物提交进仓库。发布期一个只用 `gaby_vm` 公共 API 的通用回放 harness，把每条 fixture 在 cache 轨 + decoder 轨上回放，做差分 oracle（两轨逐项对拍）+ 绝对 oracle（满足 VIXL 采集的 ASSERT 目标）。

**Tech Stack:** C++17/20、CMake、CTest、VIXL（参考树在 `../vixl`，仅编写期工具链接）、gaby_vm 公共 API。

> **设计依据**：[`gaby-vm-vixl-sim-test-port-design-2026-06-08.md`](./gaby-vm-vixl-sim-test-port-design-2026-06-08.md)。本计划是它的可执行落地。所有签名/宏体均来自对源码的逐字抽取，无占位符。

---

## 关键事实速查（实现时按这个对接口，不要凭记忆）

**gaby_vm 公共 API**（`Sources/gaby_vm/include/gaby_vm/`，namespace `gaby_vm`）：
- `Simulator(PredecodeCache* cache, void* stack_buffer, size_t stack_size);`（cache 可为 null = 仅 decoder 轨）
- `void RunFrom(uintptr_t entry_pc);` / `void DebugRunFrom(uintptr_t entry_pc);`
- `void Write(GpRegister reg, uint64_t);` / `uint64_t Read(GpRegister) const;`
- `void WriteAll(const RegisterFile&);` / `RegisterFile ReadAll() const;`
- `void Write(VRegister, VRegisterValue);` / `Read(VRegister)`；`Write(SysRegister, uint32_t)` / `Read(SysRegister)`
- `static constexpr size_t kMinStackSize = 12 * 1024;`
- `GpRegister`：`X0..X30`，`LR=X30`，`SP=31`，`PC=32`（`enum class : uint8_t`）
- `SysRegister`：`NZCV=0, FPCR=1, FPSR=2, BType=3`
- `struct RegisterFile { uint64_t x[31]; uint64_t sp; uint64_t pc; VRegisterValue v[32]; uint32_t nzcv; uint32_t fpcr; uint32_t fpsr; uint32_t btype; };`
- `struct VRegisterValue { uint64_t lo; uint64_t hi; };`
- `PredecodeCache::RegisterCodeRange(const void* start, size_t size_bytes) -> RegistrationStatus{Ok,InvalidArgument,OverlappingRange,UnsupportedFeature,OutOfMemory}`
- 终止契约：构造时 LR 置 null 哨兵（0），`RET` 到 null LR 即停。仿真在 PC 变为 0 时结束。

**`test/embedding_stack.h`**：`struct StackBuffer { alignas(16) std::array<uint8_t, 16*1024> bytes{}; void* data(); size_t size(); };`

**现有测试范式**（`test/simulator_correctness.cc`）：`run_dual(s, name, code, words, setup, verify)`；入口 `const uintptr_t entry = reinterpret_cast<uintptr_t>(code);`；cache 轨建 `PredecodeCache` + `RegisterCodeRange` + `Simulator(&cache,...)` + `RunFrom`；debug 轨 `Simulator(nullptr,...)` + `DebugRunFrom`。

**CMake 范式**：
- 公共 API 测试：`add_executable` + `target_link_libraries(t PRIVATE gaby_vm::gaby_vm)` + `gaby_vm_apply_compile_flags(t)` + `add_test`。
- 特权测试（够到 vixl 头）：额外 `target_include_directories(t PRIVATE ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src)` + `target_compile_definitions(t PRIVATE VIXL_INCLUDE_TARGET_A64 VIXL_INCLUDE_SIMULATOR_AARCH64 $<$<CONFIG:Debug>:VIXL_DEBUG>)`。
- 顶层 option 范式：`option(GABY_VM_BUILD_NATIVE_BASELINE "..." OFF)`；`CMakeLists.txt:21-36` 串子目录。
- `gaby_vm_apply_compile_flags` 定义在 `cmake/CompileFlags.cmake:5`。

**VIXL 测试宏**（`../vixl/test/aarch64/test-assembler-aarch64.h`，simulator 变体；变量名固定）：
- `#define __ masm.`
- `SETUP()` 造 `MacroAssembler masm;` + `SETUP_COMMON()`（造 `Decoder simulator_decoder; RegisterDump core; ptrdiff_t offset_after_infrastructure_start; ptrdiff_t offset_before_infrastructure_end;`）+ `SETUP_COMMON_SIM()`（造 `Simulator simulator(&simulator_decoder);`）。
- `SETUP_WITH_FEATURES(...)`：同上，外加 `masm.SetCPUFeatures(CPUFeatures(__VA_ARGS__)); simulator.SetCPUFeatures(CPUFeatures(__VA_ARGS__));`
- `START()`：`masm.Reset(); simulator.ResetState();` → `PushCalleeSavedRegisters()` → `offset_after_infrastructure_start = masm.GetCursorOffset();`
- `END()`：`offset_before_infrastructure_end = masm.GetCursorOffset();` → trace off → `core.Dump(&masm); __ PopCalleeSavedRegisters(); __ Ret(); masm.FinalizeCode();`
- `RUN()` → `RUN_WITHOUT_SEEN_FEATURE_CHECK()` → `simulator.RunFrom(masm.GetBuffer()->GetStartAddress<Instruction*>());`
- `ASSERT_EQUAL_64(expected, result) -> VIXL_CHECK(Equal64(expected, &core, result))`；`result` 是 `Register`（如 `x1`，可取 `.GetCode()`）。同族：`ASSERT_EQUAL_32 / _NZCV / _FP32 / _FP64 / _128`。
- `TEST(Name)` → `TEST_(Name)` → `void TestName(); Test test_Name(#Name,&TestName); void TestName()`（`Test` 构造时把自己挂进静态链表 `Test::first()`）。
- buffer：`masm.GetBuffer()->GetStartAddress<T>()`；`masm.GetCursorOffset()`；`masm.GetBuffer()->GetOffsetAddress<T>(offset)`。
- 采集后状态：`simulator.SetCPUFeatures/GetSeenFeatures`；`CPUFeaturesAuditor`（`../vixl/src/aarch64/cpu-features-auditor-aarch64.h`）有 `GetSeenFeatures()`、可挂到 `Decoder`。

**过滤事实**：gaby-vm 内部用 `CPUFeatures::All()`，所以特性不会卡注册；真正会炸的是 39 个 `VisitUnimplemented` 形态（`VIXL_UNIMPLEMENTED()` 在 Debug 下 abort）。过滤在编写期做。

---

## 文件结构

**编写期工具（默认不构建，藏在 `-DGABY_VM_BUILD_VIXL_EXTRACT=ON` + `-DVIXL_SRC_DIR=../vixl` 后）**
- `tools/vixl_test_extract/capture_macros.h` — 重定义 `TEST/SETUP*/START/END/RUN/ASSERT_EQUAL_*`，把每个 TEST 变成「采集」。唯一有点 trick 的文件。
- `tools/vixl_test_extract/capture_state.h` / `.cc` — 采集状态容器（当前 fixture 的字节、入口寄存器、assert 目标、特性）；`RegisterDump` → `gaby_vm::RegisterFile` 映射。
- `tools/vixl_test_extract/fixture_writer.h` / `.cc` — 把采集结果序列化成 `*.inc`（`constexpr` C++ 数据）。
- `tools/vixl_test_extract/extract_main.cc` — 遍历 `Test::first()` 链表，逐个采集、过滤、写出，并打印「纳入/丢弃」报告。
- `tools/vixl_test_extract/phase0_sample_tests.cc` — Phase 0 用的小批 VIXL 风格测试（~6 条整数），证明管线端到端。
- `tools/vixl_test_extract/CMakeLists.txt` — 工具 target，链接 `../vixl`。

**发布期回放（自包含，进 CI/iOS）**
- `test/vixl_port/vixl_port_fixture.h` — `PortedFixture` / `AssertTarget` POD 定义（无 vixl 依赖）。
- `test/vixl_port/vixl_port_runner.h` / `.cc` — 通用回放 + 双 oracle。公共 API only。
- `test/vixl_port/generated/integer_fixtures.inc`（Phase 1）、`fp_fixtures.inc`（Phase 2）、`neon_fixtures.inc`（Phase 3）— **生成并提交**。
- `test/vixl_port/generated/manifest_<family>.md` — 纳入/丢弃报告，**提交**。
- `test/vixl_port/vixl_port_integer.cc` / `vixl_port_fp.cc` / `vixl_port_neon.cc` — 三个 CTest main。
- `test/vixl_port/CMakeLists.txt` — 注册三个测试 + Phase 0 自检。
- `test/CMakeLists.txt` — 加一行 `add_subdirectory(vixl_port)`。

**顶层**
- `CMakeLists.txt` — 加 `option(GABY_VM_BUILD_VIXL_EXTRACT ...)` 与条件 `add_subdirectory(tools/vixl_test_extract)`。

---

## Phase 0 — 打通管线（在 ~6 条整数用例上端到端，含注入缺陷自检）

> Phase 0 只跑整数，`RegisterFile` 映射只用 `x[0..30]`、`sp`、`nzcv`，`v[]`/`fp` 留零。这样把「采集→生成→回放→双 oracle」全链路证实，再在后续 phase 扩到 FP/NEON。

### Task 0.1：fixture 数据结构（无依赖 POD）

**Files:**
- Create: `test/vixl_port/vixl_port_fixture.h`

- [ ] **Step 1：写文件**

```cpp
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// POD fixture types shared by generated *.inc data and the replay runner.
// No VIXL types here: this header is part of the self-contained, shipping
// test surface and must compile with only the gaby_vm public API in scope.
#ifndef GABY_VM_TEST_VIXL_PORT_FIXTURE_H_
#define GABY_VM_TEST_VIXL_PORT_FIXTURE_H_

#include <cstddef>
#include <cstdint>

#include "gaby_vm/registers.h"

namespace gaby_vm::vixl_port {

// One ASSERT_EQUAL_* target harvested from the upstream VIXL test.
enum class AssertKind : uint8_t {
  kX,      // 64-bit GP register (ASSERT_EQUAL_64 on an X register)
  kW,      // 32-bit GP register (ASSERT_EQUAL_32 on a W register)
  kNZCV,   // condition flags (ASSERT_EQUAL_NZCV)
  kFP32,   // single-precision (ASSERT_EQUAL_FP32 on an S register), raw bits in expected_lo
  kFP64,   // double-precision (ASSERT_EQUAL_FP64 on a D register), raw bits in expected_lo
  kV128,   // 128-bit vector (ASSERT_EQUAL_128), expected_hi:expected_lo
};

struct AssertTarget {
  AssertKind kind;
  uint8_t reg;            // register code 0..31 (ignored for kNZCV)
  uint64_t expected_lo;
  uint64_t expected_hi;   // only meaningful for kV128
};

// One ported test case. `code` points at body words followed by a trailing
// RET (0xd65f03c0) so RunFrom terminates on the null-LR contract. `entry` is
// the architectural state at body entry (sp/LR are overridden at replay).
struct PortedFixture {
  const char* name;
  const uint32_t* code;
  size_t code_words;
  RegisterFile entry;
  const AssertTarget* asserts;
  size_t assert_count;
};

}  // namespace gaby_vm::vixl_port

#endif  // GABY_VM_TEST_VIXL_PORT_FIXTURE_H_
```

- [ ] **Step 2：提交**

```bash
git add test/vixl_port/vixl_port_fixture.h
git commit -m "test(vixl-port): add POD fixture types for ported VIXL cases"
```

### Task 0.2：回放 runner 的失败测试（TDD red）

**Files:**
- Create: `test/vixl_port/vixl_port_runner.h`
- Create: `test/vixl_port/generated/phase0_handwritten.inc`（Phase 0 临时手写 fixture，证明 runner；Phase 1 由工具生成真数据）
- Create: `test/vixl_port/vixl_port_integer.cc`
- Create: `test/vixl_port/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1：先声明 runner 接口（仅头，未实现）**

`test/vixl_port/vixl_port_runner.h`：

```cpp
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Generic replay harness for ported VIXL simulator tests. Runs each fixture
// on both gaby_vm tracks (cache via RunFrom, decoder via DebugRunFrom) and
// applies two oracles:
//   - differential: full RegisterFile must match between the two tracks;
//   - absolute: every harvested ASSERT_EQUAL_* target must hold on each track.
// Uses only the gaby_vm public API.
#ifndef GABY_VM_TEST_VIXL_PORT_RUNNER_H_
#define GABY_VM_TEST_VIXL_PORT_RUNNER_H_

#include <cstddef>

#include "vixl_port_fixture.h"

namespace gaby_vm::vixl_port {

struct RunStats {
  int cases = 0;
  int passed = 0;   // a case passes only if BOTH oracles hold on BOTH tracks
};

// Runs one fixture; prints [FAIL] detail to stderr on any oracle violation.
// Returns true iff the case fully passes.
bool RunFixture(const PortedFixture& fx);

// Runs an array of fixtures, accumulating into stats.
void RunAll(const PortedFixture* fixtures, size_t count, RunStats& stats);

}  // namespace gaby_vm::vixl_port

#endif  // GABY_VM_TEST_VIXL_PORT_RUNNER_H_
```

- [ ] **Step 2：写一个手写 fixture（已知正确）+ 注入缺陷开关**

`test/vixl_port/generated/phase0_handwritten.inc`：

```cpp
// Phase-0 hand-authored fixtures (NOT machine-generated). They exercise the
// replay runner before the extraction tool exists. Phase 1 replaces the
// machine-generated integer set; this file is removed at the end of Phase 1.
//
// Encodings verified with: clang -target aarch64 -c -x assembler / objdump -d.
#include "vixl_port_fixture.h"

namespace gaby_vm::vixl_port {
namespace {

// add x0, x1, x2 ; ret
constexpr uint32_t kAddCode[] = {0x8b020020u, 0xd65f03c0u};
constexpr AssertTarget kAddAsserts[] = {
    // X1=2, X2=3 (see entry) -> X0 == 5
    {AssertKind::kX, /*reg=*/0, /*lo=*/5u, /*hi=*/0u},
};

RegisterFile MakeAddEntry() {
  RegisterFile rf{};   // value-initialised: all zero
  rf.x[1] = 2u;
  rf.x[2] = 3u;
  return rf;
}

}  // namespace

// Exposed for the integer main. MakeAddEntry() called at startup.
inline const PortedFixture* Phase0Fixtures(size_t* count) {
  static const RegisterFile add_entry = MakeAddEntry();
  static const PortedFixture fixtures[] = {
      {"hand/add_x0_x1_x2", kAddCode, 2, add_entry, kAddAsserts, 1},
  };
  *count = sizeof(fixtures) / sizeof(fixtures[0]);
  return fixtures;
}

}  // namespace gaby_vm::vixl_port
```

`test/vixl_port/vixl_port_integer.cc`：

```cpp
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// CTest entry for ported integer-family VIXL tests. In Phase 0 it runs the
// hand-authored fixtures; from Phase 1 it includes the generated integer set.
#include <cstdio>
#include <cstdlib>

#include "vixl_port_runner.h"

// Phase 0: hand-authored fixtures. Replaced by generated include in Phase 1.
#include "generated/phase0_handwritten.inc"

int main() {
  using namespace gaby_vm::vixl_port;
  size_t count = 0;
  const PortedFixture* fixtures = Phase0Fixtures(&count);

  RunStats stats;
  RunAll(fixtures, count, stats);

  std::printf("vixl_port_integer: %d/%d ported cases passed\n",
              stats.passed, stats.cases);
  return (stats.passed == stats.cases) ? 0 : 1;
}
```

`test/vixl_port/CMakeLists.txt`：

```cmake
add_executable(vixl_port_integer
  vixl_port_integer.cc
  vixl_port_runner.cc)
target_link_libraries(vixl_port_integer PRIVATE gaby_vm::gaby_vm)
target_include_directories(vixl_port_integer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gaby_vm_apply_compile_flags(vixl_port_integer)
add_test(NAME vixl_port_integer COMMAND vixl_port_integer)
```

`test/CMakeLists.txt`（追加一行，放在文件末尾）：

```cmake
add_subdirectory(vixl_port)
```

- [ ] **Step 3：构建并确认 link 失败（runner 未实现）**

Run: `cmake --preset dev-debug && cmake --build build/debug --target vixl_port_integer`
Expected: 链接错误 `undefined reference to gaby_vm::vixl_port::RunAll` / `RunFixture`（red：接口已声明、实现未写）。

### Task 0.3：实现 runner（TDD green）

**Files:**
- Create: `test/vixl_port/vixl_port_runner.cc`

- [ ] **Step 1：写实现**

```cpp
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
#include "vixl_port_runner.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "embedding_stack.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/simulator.h"

namespace gaby_vm::vixl_port {
namespace {

constexpr uint32_t kRet = 0xd65f03c0u;
constexpr uint64_t kNullLr = 0u;  // RET to null LR terminates the run.

// 16-byte-down-aligned top of a StackBuffer, used as the replay SP.
uint64_t StackTop(const StackBuffer& stack) {
  auto base = reinterpret_cast<uintptr_t>(stack.data());
  uintptr_t top = base + stack.size();
  return static_cast<uint64_t>(top & ~uintptr_t{15});
}

// Seats entry state on a freshly constructed Simulator: full RegisterFile
// from the fixture, then sp -> this process's stack, LR -> null sentinel.
void SeatEntry(Simulator& sim, const PortedFixture& fx, const StackBuffer& stk) {
  sim.WriteAll(fx.entry);
  sim.Write(GpRegister::SP, StackTop(stk));
  sim.Write(GpRegister::LR, kNullLr);
}

bool CheckAssert(const PortedFixture& fx,
                 const AssertTarget& a,
                 Simulator& sim,
                 const char* track) {
  uint64_t got_lo = 0, got_hi = 0, want_lo = a.expected_lo, want_hi = 0;
  const char* what = "?";
  switch (a.kind) {
    case AssertKind::kX:
      got_lo = sim.Read(static_cast<GpRegister>(a.reg));
      what = "X";
      break;
    case AssertKind::kW:
      got_lo = sim.Read(static_cast<GpRegister>(a.reg)) & 0xffffffffu;
      want_lo = a.expected_lo & 0xffffffffu;
      what = "W";
      break;
    case AssertKind::kNZCV:
      got_lo = sim.Read(SysRegister::NZCV);
      what = "NZCV";
      break;
    case AssertKind::kFP32: {
      VRegisterValue v = sim.Read(static_cast<VRegister>(a.reg));
      got_lo = v.lo & 0xffffffffu;
      want_lo = a.expected_lo & 0xffffffffu;
      what = "S";
      break;
    }
    case AssertKind::kFP64: {
      VRegisterValue v = sim.Read(static_cast<VRegister>(a.reg));
      got_lo = v.lo;
      what = "D";
      break;
    }
    case AssertKind::kV128: {
      VRegisterValue v = sim.Read(static_cast<VRegister>(a.reg));
      got_lo = v.lo;
      got_hi = v.hi;
      want_hi = a.expected_hi;
      what = "V";
      break;
    }
  }
  if (got_lo == want_lo && got_hi == want_hi) return true;
  std::fprintf(stderr,
               "[FAIL] %s / absolute / %s track: %s%u\n"
               "  actual   = 0x%016" PRIx64 ":%016" PRIx64 "\n"
               "  expected = 0x%016" PRIx64 ":%016" PRIx64 "\n",
               fx.name, track, what, a.reg, got_hi, got_lo, want_hi, want_lo);
  return false;
}

// Runs the fixture body+RET on one track and returns the post-run RegisterFile.
// `use_cache` selects RunFrom (cache) vs DebugRunFrom (decoder).
bool RunOneTrack(const PortedFixture& fx,
                 bool use_cache,
                 RegisterFile& out_state,
                 const char* track) {
  // Body words followed by a trailing RET in a contiguous buffer.
  static thread_local std::vector<uint32_t> buf;
  buf.assign(fx.code, fx.code + fx.code_words);
  buf.push_back(kRet);
  const uintptr_t entry = reinterpret_cast<uintptr_t>(buf.data());
  const size_t size_bytes = buf.size() * sizeof(uint32_t);

  StackBuffer stack;
  if (use_cache) {
    PredecodeCache cache;
    auto st = cache.RegisterCodeRange(buf.data(), size_bytes);
    if (st != PredecodeCache::RegistrationStatus::Ok) {
      std::fprintf(stderr,
                   "[FAIL] %s / %s track: RegisterCodeRange failed (status=%d)\n",
                   fx.name, track, static_cast<int>(st));
      return false;
    }
    Simulator sim(&cache, stack.data(), stack.size());
    SeatEntry(sim, fx, stack);
    sim.RunFrom(entry);
    out_state = sim.ReadAll();
    bool ok = true;
    for (size_t i = 0; i < fx.assert_count; ++i)
      ok &= CheckAssert(fx, fx.asserts[i], sim, track);
    return ok;
  }
  Simulator sim(nullptr, stack.data(), stack.size());
  SeatEntry(sim, fx, stack);
  sim.DebugRunFrom(entry);
  out_state = sim.ReadAll();
  bool ok = true;
  for (size_t i = 0; i < fx.assert_count; ++i)
    ok &= CheckAssert(fx, fx.asserts[i], sim, track);
  return ok;
}

// Differential oracle: compares the two tracks' full RegisterFile.
bool DifferentialEqual(const PortedFixture& fx,
                       const RegisterFile& a,
                       const RegisterFile& b) {
  bool ok = true;
  auto diff = [&](const char* name, uint64_t x, uint64_t y) {
    if (x == y) return;
    std::fprintf(stderr,
                 "[FAIL] %s / differential (cache vs decoder): %s\n"
                 "  cache   = 0x%016" PRIx64 "\n"
                 "  decoder = 0x%016" PRIx64 "\n",
                 fx.name, name, x, y);
    ok = false;
  };
  for (int i = 0; i < 31; ++i) diff("x", a.x[i], b.x[i]);
  diff("sp", a.sp, b.sp);
  for (int i = 0; i < 32; ++i) {
    diff("v.lo", a.v[i].lo, b.v[i].lo);
    diff("v.hi", a.v[i].hi, b.v[i].hi);
  }
  diff("nzcv", a.nzcv, b.nzcv);
  diff("fpcr", a.fpcr, b.fpcr);
  diff("fpsr", a.fpsr, b.fpsr);
  // pc/btype intentionally excluded: pc is the terminal sentinel; btype is a
  // transient BTI tracker not asserted by upstream VIXL.
  return ok;
}

}  // namespace

bool RunFixture(const PortedFixture& fx) {
  RegisterFile cache_state{}, decoder_state{};
  bool cache_ok = RunOneTrack(fx, /*use_cache=*/true, cache_state, "cache");
  bool decoder_ok = RunOneTrack(fx, /*use_cache=*/false, decoder_state, "decoder");
  bool diff_ok = DifferentialEqual(fx, cache_state, decoder_state);
  return cache_ok && decoder_ok && diff_ok;
}

void RunAll(const PortedFixture* fixtures, size_t count, RunStats& stats) {
  for (size_t i = 0; i < count; ++i) {
    stats.cases += 1;
    if (RunFixture(fixtures[i])) stats.passed += 1;
  }
}

}  // namespace gaby_vm::vixl_port
```

> 注：`vixl_port_runner.cc` 用了 `<vector>` 与 `embedding_stack.h`，编译时需要把 `${PROJECT_SOURCE_DIR}/test` 加进 include 路径以找到 `embedding_stack.h`。在 `test/vixl_port/CMakeLists.txt` 的 `target_include_directories` 行追加 `${PROJECT_SOURCE_DIR}/test`，并在本文件顶部 `#include <vector>`。

- [ ] **Step 2：补 include 路径与头**

在 `test/vixl_port/vixl_port_runner.cc` 顶部 include 区加入 `#include <vector>`。
在 `test/vixl_port/CMakeLists.txt` 的 `target_include_directories(vixl_port_integer PRIVATE ...)` 后追加路径：

```cmake
target_include_directories(vixl_port_integer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/test)
```

- [ ] **Step 3：构建并运行，确认 green**

Run: `cmake --build build/debug --target vixl_port_integer && ctest --test-dir build/debug -R vixl_port_integer --output-on-failure`
Expected: PASS，输出 `vixl_port_integer: 1/1 ported cases passed`。

- [ ] **Step 4：注入缺陷自检（证明 guard rail 会叫）**

临时把 `kAddAsserts` 的期望从 `5u` 改成 `6u`，重跑：

Run: `cmake --build build/debug --target vixl_port_integer && ctest --test-dir build/debug -R vixl_port_integer --output-on-failure`
Expected: FAIL，stderr 出现 `[FAIL] hand/add_x0_x1_x2 / absolute / cache track: X0`。确认后改回 `5u`，重跑应 PASS。

- [ ] **Step 5：提交**

```bash
git add test/vixl_port/ test/CMakeLists.txt
git commit -m "test(vixl-port): replay runner with differential+absolute oracles (phase 0)"
```

### Task 0.4：提取工具骨架 + capture_macros（编写期）

**Files:**
- Create: `tools/vixl_test_extract/capture_state.h`
- Create: `tools/vixl_test_extract/capture_macros.h`
- Create: `tools/vixl_test_extract/phase0_sample_tests.cc`
- Create: `tools/vixl_test_extract/extract_main.cc`
- Create: `tools/vixl_test_extract/fixture_writer.h` / `.cc`
- Create: `tools/vixl_test_extract/CMakeLists.txt`
- Modify: `CMakeLists.txt`（顶层）

- [ ] **Step 1：采集状态容器**

`tools/vixl_test_extract/capture_state.h`：

```cpp
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Authorship-time only. Links the reference VIXL (Tier-0 MacroAssembler +
// Simulator). NEVER part of the gaby-vm build/runtime.
#ifndef GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_STATE_H_
#define GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_STATE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace gaby_vm::extract {

enum class AssertKind : uint8_t { kX, kW, kNZCV, kFP32, kFP64, kV128 };

struct AssertTarget {
  AssertKind kind;
  uint8_t reg;
  uint64_t expected_lo;
  uint64_t expected_hi;
};

// Mirror of gaby_vm::RegisterFile, kept local so this tool does not need the
// gaby_vm headers (it links VIXL, not gaby_vm).
struct EntryState {
  uint64_t x[31] = {};
  uint64_t sp = 0;
  uint64_t v_lo[32] = {};
  uint64_t v_hi[32] = {};
  uint32_t nzcv = 0;
  uint32_t fpcr = 0;
  uint32_t fpsr = 0;
};

struct CapturedCase {
  std::string name;
  std::vector<uint32_t> body_words;   // body only (epilogue excluded)
  EntryState entry;
  std::vector<AssertTarget> asserts;
  std::string required_features;      // human-readable, from the auditor
  bool skipped = false;
  std::string skip_reason;
};

// The single in-flight case being captured by the macros. extract_main resets
// it before invoking each TEST callback, then reads it out.
CapturedCase& Current();
void ResetCurrent(const char* name);

}  // namespace gaby_vm::extract

#endif  // GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_STATE_H_
```

- [ ] **Step 2：capture_macros — 重定义 VIXL 测试宏**

`tools/vixl_test_extract/capture_macros.h`：

```cpp
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Redefines VIXL's test macros so that a VIXL TEST() body, compiled verbatim,
// captures bytes + entry state + ASSERT targets instead of running assertions.
//
// IMPORTANT: include this BEFORE any VIXL test .cc. It pre-occupies the
// include guard of test-assembler-aarch64.h so VIXL's own macro definitions
// are skipped, then provides the declarations a test body needs.
#ifndef GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_MACROS_H_
#define GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_MACROS_H_

#include "aarch64/macro-assembler-aarch64.h"
#include "aarch64/simulator-aarch64.h"
#include "aarch64/cpu-features-auditor-aarch64.h"
#include "test-runner.h"
#include "aarch64/test-utils-aarch64.h"   // RegisterDump, Equal* (used to self-verify)

#include "capture_state.h"

// Block VIXL's test-assembler-aarch64.h macros: pre-define its include guard.
// (Confirm the exact guard token in the header; adjust if upstream differs.)
#define VIXL_TEST_AARCH64_TEST_ASSEMBLER_AARCH64_H_

namespace vixl {
namespace aarch64 {

// Per-case captured machinery, replacing SETUP()'s locals.
struct CaptureRig {
  MacroAssembler masm;
  Decoder decoder;
  Simulator simulator{&decoder};
  RegisterDump entry_core;   // dumped right after the prologue (body entry)
  RegisterDump exit_core;    // dumped at END (body exit) — VIXL's `core`
  ptrdiff_t body_start = 0;
  ptrdiff_t body_end = 0;
};

}  // namespace aarch64
}  // namespace vixl

#define __ rig.masm.

// SETUP family: build a CaptureRig instead of bare locals.
#define SETUP() vixl::aarch64::CaptureRig rig
#define SETUP_WITH_FEATURES(...)                                  \
  vixl::aarch64::CaptureRig rig;                                  \
  rig.masm.SetCPUFeatures(vixl::CPUFeatures(__VA_ARGS__));        \
  rig.simulator.SetCPUFeatures(vixl::CPUFeatures(__VA_ARGS__))

// START: real prologue, then dump entry state, then record body start.
#define START()                                            \
  rig.masm.Reset();                                        \
  rig.simulator.ResetState();                              \
  rig.masm.PushCalleeSavedRegisters();                     \
  rig.entry_core.Dump(&rig.masm);                          \
  rig.body_start = rig.masm.GetCursorOffset()

// END: record body end, dump exit state, epilogue, finalize, then capture.
#define END()                                              \
  rig.body_end = rig.masm.GetCursorOffset();               \
  rig.exit_core.Dump(&rig.masm);                           \
  rig.masm.PopCalleeSavedRegisters();                      \
  rig.masm.Ret();                                          \
  rig.masm.FinalizeCode()

// RUN: execute on the real Simulator, then harvest bytes + entry state.
#define RUN() ::gaby_vm::extract::HarvestRun(rig)

// ASSERT_EQUAL_*: record the target (and self-verify against exit_core).
#define ASSERT_EQUAL_64(expected, result) \
  ::gaby_vm::extract::RecordX(rig, (expected), (result).GetCode())
#define ASSERT_EQUAL_32(expected, result) \
  ::gaby_vm::extract::RecordW(rig, (expected), (result).GetCode())
#define ASSERT_EQUAL_NZCV(expected) \
  ::gaby_vm::extract::RecordNzcv(rig, (expected))
#define ASSERT_EQUAL_FP32(expected, result) \
  ::gaby_vm::extract::RecordFP32(rig, (expected), (result).GetCode())
#define ASSERT_EQUAL_FP64(expected, result) \
  ::gaby_vm::extract::RecordFP64(rig, (expected), (result).GetCode())
#define ASSERT_EQUAL_128(expected_h, expected_l, result) \
  ::gaby_vm::extract::RecordV128(rig, (expected_h), (expected_l), (result).GetCode())

// TEST(Name): register a callback exactly like VIXL, so extract_main can walk
// the Test linked list. (Reuses the real Test class from test-runner.h.)
#define TEST(Name)                                       \
  void Test##Name();                                     \
  vixl::Test test_##Name(#Name, &Test##Name);            \
  void Test##Name()

namespace gaby_vm {
namespace extract {

// Declarations implemented in capture_runtime.cc (Task 0.4 Step 3).
void HarvestRun(vixl::aarch64::CaptureRig& rig);
void RecordX(vixl::aarch64::CaptureRig& rig, uint64_t expected, unsigned code);
void RecordW(vixl::aarch64::CaptureRig& rig, uint32_t expected, unsigned code);
void RecordNzcv(vixl::aarch64::CaptureRig& rig, uint32_t expected);
void RecordFP32(vixl::aarch64::CaptureRig& rig, float expected, unsigned code);
void RecordFP64(vixl::aarch64::CaptureRig& rig, double expected, unsigned code);
void RecordV128(vixl::aarch64::CaptureRig& rig, uint64_t eh, uint64_t el, unsigned code);

}  // namespace extract
}  // namespace gaby_vm

#endif  // GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_MACROS_H_
```

> **R1 风险点**：`VIXL_TEST_AARCH64_TEST_ASSEMBLER_AARCH64_H_` 必须等于 `test-assembler-aarch64.h` 的真实 include guard。实现时先 `grep -n "#ifndef" ../vixl/test/aarch64/test-assembler-aarch64.h | head -1` 确认，写错会导致宏重复定义冲突。

- [ ] **Step 3：capture 运行时（HarvestRun / Record*）**

`tools/vixl_test_extract/capture_state.cc`（与 capture_state.h 同名实现 + Harvest/Record）：

```cpp
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
#include "capture_state.h"
#include "capture_macros.h"

namespace gaby_vm::extract {

namespace {
CapturedCase g_current;
}

CapturedCase& Current() { return g_current; }
void ResetCurrent(const char* name) {
  g_current = CapturedCase{};
  g_current.name = name;
}

void HarvestRun(vixl::aarch64::CaptureRig& rig) {
  using vixl::aarch64::Instruction;
  // Execute the full emitted program on the real VIXL simulator.
  rig.simulator.RunFrom(
      rig.masm.GetBuffer()->GetStartAddress<Instruction*>());

  CapturedCase& c = Current();

  // Body bytes: [body_start, body_end), word by word.
  auto* base = rig.masm.GetBuffer()->GetStartAddress<const uint32_t*>();
  size_t first = static_cast<size_t>(rig.body_start) / sizeof(uint32_t);
  size_t last = static_cast<size_t>(rig.body_end) / sizeof(uint32_t);
  for (size_t i = first; i < last; ++i) c.body_words.push_back(base[i]);

  // Entry state from entry_core (dumped right after the prologue).
  for (unsigned i = 0; i < 31; ++i)
    c.entry.x[i] = static_cast<uint64_t>(rig.entry_core.xreg(i));
  c.entry.sp = static_cast<uint64_t>(rig.entry_core.xreg(31));  // SP code
  for (unsigned i = 0; i < 32; ++i) {
    vixl::aarch64::QRegisterValue q = rig.entry_core.qreg(i);
    c.entry.v_lo[i] = q.GetLane<uint64_t>(0);
    c.entry.v_hi[i] = q.GetLane<uint64_t>(1);
  }
  c.entry.nzcv = rig.entry_core.flags_nzcv();

  // Required features the body actually used.
  std::ostringstream os;
  os << rig.simulator.GetSeenFeatures();
  c.required_features = os.str();
}

void RecordX(vixl::aarch64::CaptureRig& rig, uint64_t expected, unsigned code) {
  // Self-verify against exit_core so we never export a case VIXL itself fails.
  if (static_cast<uint64_t>(rig.exit_core.xreg(code)) != expected) {
    Current().skipped = true;
    Current().skip_reason = "self-check: X assert mismatch in VIXL";
  }
  Current().asserts.push_back(
      {AssertKind::kX, static_cast<uint8_t>(code), expected, 0});
}
// RecordW / RecordNzcv / RecordFP32 / RecordFP64 / RecordV128: same shape,
// reading exit_core.wreg / flags_nzcv / sreg_bits / dreg_bits / qreg.
// (Implement each mirroring RecordX, pushing the corresponding AssertKind.)

}  // namespace gaby_vm::extract
```

> 注：`QRegisterValue::GetLane<uint64_t>(n)` 的确切取半 API 在 Phase 0 用不到（整数用例 v[] 留零），实现时按 `../vixl/src/aarch64/simulator-aarch64.h` 的 `QRegisterValue` 真实接口对齐；Phase 3（NEON）前必须验证。Phase 0 可先只填 x/sp/nzcv，v 留零。

- [ ] **Step 4：Phase 0 样本测试（VIXL 风格，验证宏吞得下）**

`tools/vixl_test_extract/phase0_sample_tests.cc`：

```cpp
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// A tiny VIXL-style test file consumed by the capture macros. Proves the
// extraction pipeline end-to-end before pointing it at real VIXL files.
#include "capture_macros.h"

namespace vixl {
namespace aarch64 {

TEST(extract_add) {
  SETUP();
  START();
  __ Mov(x1, 2);
  __ Mov(x2, 3);
  __ Add(x0, x1, x2);
  END();
  RUN();
  ASSERT_EQUAL_64(5, x0);
}

TEST(extract_sub_flags) {
  SETUP();
  START();
  __ Mov(x1, 7);
  __ Subs(x0, x1, 7);   // sets Z
  END();
  RUN();
  ASSERT_EQUAL_64(0, x0);
}

}  // namespace aarch64
}  // namespace vixl
```

- [ ] **Step 5：extract_main + fixture_writer**

`tools/vixl_test_extract/extract_main.cc`（遍历 Test 链表、采集、过滤、写出、报告）：

```cpp
// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
#include <cstdio>
#include <string>

#include "test-runner.h"
#include "capture_state.h"
#include "fixture_writer.h"

// Feature-name substrings gaby-vm cannot execute (the 39 VisitUnimplemented
// forms cluster here). A captured case whose required_features mentions any of
// these is skipped. Refined empirically in Phase 1.
static const char* kDenyFeatureSubstrings[] = {
    "MTE", "BFloat", "BF16", "TME", "WFXT",
};

int main(int argc, char** argv) {
  // argv[1] = output .inc path; argv[2] = symbol prefix (e.g. "Integer").
  const std::string out_path = (argc > 1) ? argv[1] : "out.inc";
  const std::string prefix = (argc > 2) ? argv[2] : "Generated";

  gaby_vm::extract::FixtureWriter writer(out_path, prefix);

  for (vixl::Test* t = vixl::Test::first(); t != nullptr; t = t->next()) {
    gaby_vm::extract::ResetCurrent(t->name());
    t->run();  // invokes the TEST body -> capture macros fill Current()
    gaby_vm::extract::CapturedCase& c = gaby_vm::extract::Current();

    for (const char* deny : kDenyFeatureSubstrings) {
      if (c.required_features.find(deny) != std::string::npos) {
        c.skipped = true;
        c.skip_reason = std::string("unsupported feature: ") + deny;
      }
    }
    writer.Add(c);
  }
  writer.Finish();
  return 0;
}
```

`fixture_writer.h` / `.cc`：把每个未跳过的 `CapturedCase` 写成 `constexpr uint32_t <prefix>_<n>_code[] = {...};`、`constexpr AssertTarget <prefix>_<n>_asserts[] = {...};`、一个 `RegisterFile` 字面量构造函数，最后聚合成 `const PortedFixture k<prefix>Fixtures[]`，并暴露 `const PortedFixture* <prefix>Fixtures(size_t* count);`。同时把跳过项写进同目录 `manifest_<family>.md`（名字 + 原因）。生成的 `.inc` `#include "vixl_port_fixture.h"`，落在 `test/vixl_port/generated/`。

- [ ] **Step 6：提取工具 CMake + 顶层 option**

`tools/vixl_test_extract/CMakeLists.txt`：

```cmake
# Authorship-time only. Requires the reference VIXL tree (VIXL_SRC_DIR) and
# links its Tier-0 MacroAssembler + Simulator. NEVER part of the shipping
# build: guarded by GABY_VM_BUILD_VIXL_EXTRACT.
if(NOT VIXL_SRC_DIR)
  message(FATAL_ERROR "vixl_test_extract requires -DVIXL_SRC_DIR=/path/to/vixl")
endif()

add_executable(vixl_test_extract
  extract_main.cc
  capture_state.cc
  fixture_writer.cc
  phase0_sample_tests.cc          # Phase 0; later: the real VIXL test .cc
  ${VIXL_SRC_DIR}/test/test-runner.cc
  ${VIXL_SRC_DIR}/test/aarch64/test-utils-aarch64.cc
  ${VIXL_SRC_DIR}/test/test-utils.cc)
target_include_directories(vixl_test_extract PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/test/vixl_port      # vixl_port_fixture.h not needed here; remove if unused
  ${VIXL_SRC_DIR}/src
  ${VIXL_SRC_DIR}/test
  ${VIXL_SRC_DIR}/test/aarch64)
target_compile_definitions(vixl_test_extract PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64)
# Link the reference VIXL static lib (built separately) or add VIXL src files.
# Determine the exact VIXL lib target / source list during Phase 0 Step 7.
```

顶层 `CMakeLists.txt`（在 option 区加一行，在子目录区加条件）：

```cmake
option(GABY_VM_BUILD_VIXL_EXTRACT
       "Build the authorship-time VIXL test extraction tool (needs VIXL_SRC_DIR)" OFF)
```

```cmake
if(GABY_VM_BUILD_VIXL_EXTRACT)
  add_subdirectory(tools/vixl_test_extract)
endif()
```

- [ ] **Step 7：构建提取工具并跑出 Phase 0 fixture**

Run:
```bash
cmake --preset dev-debug -DGABY_VM_BUILD_VIXL_EXTRACT=ON -DVIXL_SRC_DIR=$PWD/../vixl
cmake --build build/debug --target vixl_test_extract
./build/debug/tools/vixl_test_extract/vixl_test_extract \
    test/vixl_port/generated/phase0_extracted.inc Phase0
```
Expected: 生成 `phase0_extracted.inc`，含 `extract_add`（X0==5）与 `extract_sub_flags`（X0==0）两条；`manifest` 报告 0 丢弃。

> 若链接报 VIXL 符号未定义：Phase 0 Step 7 现场确定 `../vixl` 的库构建方式（多半是先 `cmake --build ../vixl` 出 `libvixl.a`，再 `target_link_libraries(vixl_test_extract PRIVATE <libvixl路径>)`），把它补进 `tools/vixl_test_extract/CMakeLists.txt`。

- [ ] **Step 8：用提取出的 fixture 替换手写 fixture，回放转绿**

把 `vixl_port_integer.cc` 的 `#include "generated/phase0_handwritten.inc"` + `Phase0Fixtures` 调用，替换成 `#include "generated/phase0_extracted.inc"` + `Phase0Fixtures`（writer 生成的同名函数）。

Run: `cmake --build build/debug --target vixl_port_integer && ctest --test-dir build/debug -R vixl_port_integer --output-on-failure`
Expected: PASS，`2/2 ported cases passed`。这证明「VIXL 真汇编器产生的字节 + 入口态 + 断言目标」在 gaby-vm 双轨上都对。

- [ ] **Step 9：提交 Phase 0**

```bash
git add tools/vixl_test_extract/ test/vixl_port/ CMakeLists.txt
git commit -m "feat(vixl-port): authorship-time extraction tool + end-to-end phase-0 pipeline"
```

**Phase 0 验收**：(a) 默认构建（无 option、无 VIXL_SRC_DIR）下 `vixl_port_integer` 绿；(b) 开 option 能生成 fixture；(c) 注入缺陷能让套件红（Task 0.3 Step 4 已证）。

---

## Phase 1 — 整数/逻辑/访存/分支铺量（test-assembler-aarch64.cc）

> 从这里开始是「重复执行同一套提取→过滤→生成→转绿→提交」的流程，不逐条枚举 280 个 TEST。

### Task 1.1：让 `#include "test-assembler-aarch64.cc"` 在 capture 头下编译

**Files:** Modify `tools/vixl_test_extract/CMakeLists.txt`、可能新增 `tools/vixl_test_extract/include_test_assembler.cc`

- [ ] **Step 1**：新建 `include_test_assembler.cc`：`#include "capture_macros.h"` 然后 `#include "aarch64/test-assembler-aarch64.cc"`（用 `VIXL_SRC_DIR` 路径）。把 `phase0_sample_tests.cc` 从工具 sources 换成本文件。
- [ ] **Step 2**：构建 `vixl_test_extract`。Expected：可能报缺声明/宏冲突（R1）。逐个解决：核对 include guard token；对 capture 头未覆盖的辅助宏（如 `CALL_TEST_*`、`SETUP_CUSTOM*`）按需在 capture 头补「采集版」或对用到它们的 TEST 采用 A3 兜底（在 `include_test_assembler.cc` 里 `#undef` 后重定义，或排除该 TEST）。
- [ ] **Step 3**：构建通过即提交（仅工具侧改动）：`git commit -m "feat(vixl-port): consume real test-assembler-aarch64.cc under capture macros"`。

### Task 1.2：生成整数族 fixture 并转绿

- [ ] **Step 1**：运行
```bash
./build/debug/tools/vixl_test_extract/vixl_test_extract \
    test/vixl_port/generated/integer_fixtures.inc Integer
```
- [ ] **Step 2**：`vixl_port_integer.cc` 切到 `#include "generated/integer_fixtures.inc"` + `IntegerFixtures(...)`，删除 Phase 0 临时 inc。
- [ ] **Step 3**：`ctest -R vixl_port_integer --output-on-failure`。对任何 FAIL（abort / 差分 / 绝对）做分诊：
  - 进程 abort（VisitUnimplemented）→ 把该 TEST 名加进 extract_main 的「按名 quarantine」列表，注明原因，重生成。
  - 差分（cache≠decoder）→ **这是真 bug 或真回归**，停下来按 [`gaby-vm-vixl-sim-test-port-design-2026-06-08.md`](./gaby-vm-vixl-sim-test-port-design-2026-06-08.md) §10「不掩盖语义差异」处理，记录并上报。
  - 绝对（含绝对地址的断言）→ writer 应已对「期望值落在代码/栈地址区间」的断言标记跳过；若漏标，补规则重生成。
  - literal-pool / LDR-literal 在 body 内导致越界 → quarantine，记原因（设计 §11 R 已预见）。
- [ ] **Step 4**：全绿后提交生成产物 + 报告：`git add test/vixl_port/generated/integer_fixtures.inc test/vixl_port/generated/manifest_integer.md test/vixl_port/vixl_port_integer.cc && git commit -m "test(vixl-port): port integer/loadstore/branch cases from test-assembler-aarch64.cc"`。

**Phase 1 验收**：`vixl_port_integer` 绿；`manifest_integer.md` 列出纳入数与每条丢弃原因；覆盖的指令族明显宽于 `simulator_correctness.cc`。

---

## Phase 2 — 浮点（test-assembler-fp-aarch64.cc）

### Task 2.1：补齐 FP 的入口态与断言映射

- [ ] **Step 1**：在 `capture_state.cc` 的 `HarvestRun` 里填好 v[]（`entry_core.qreg(i)` 的两半）与（如有需要）FPCR/FPSR 入口值；实现 `RecordFP32/RecordFP64`（读 `exit_core.sreg_bits/dreg_bits` 自检）。
- [ ] **Step 2**：在 `vixl_port_runner.cc` 确认 `AssertKind::kFP32/kFP64` 分支按 raw bits 比较（NaN 用位等价，已是位比较，正确）。
- [ ] **Step 3**：新增 `test/vixl_port/vixl_port_fp.cc`（mirror integer main，include `generated/fp_fixtures.inc`）+ `test/vixl_port/CMakeLists.txt` 注册 `vixl_port_fp`。

### Task 2.2：生成 FP fixture 并转绿

- [ ] **Step 1**：工具 sources 换成 include `test-assembler-fp-aarch64.cc`，构建。
- [ ] **Step 2**：`vixl_test_extract test/vixl_port/generated/fp_fixtures.inc Fp`。
- [ ] **Step 3**：`ctest -R vixl_port_fp --output-on-failure`，同 Phase 1 Step 3 分诊（FP 额外注意 FPCR 默认舍入模式：若某测试改了舍入模式而入口态没捕获 FPCR，会差分——把 FPCR 纳入入口态捕获）。
- [ ] **Step 4**：全绿提交（fixture + manifest + main + CMake）。

**Phase 2 验收**：`vixl_port_fp` 绿；FPCR 相关用例已正确捕获或记录跳过。

---

## Phase 3 — NEON（test-assembler-neon-aarch64.cc）

### Task 3.1：确认 128 位向量捕获与比较

- [ ] **Step 1**：核对 `QRegisterValue` 取两半的真实 API（`../vixl/src/aarch64/simulator-aarch64.h`），在 `HarvestRun` 填 v_lo/v_hi，实现 `RecordV128`。
- [ ] **Step 2**：`vixl_port_runner.cc` 的 `kV128` 分支已按 lo+hi 比较；差分 oracle 已逐 v[] 对拍 lo/hi。确认无误。
- [ ] **Step 3**：新增 `test/vixl_port/vixl_port_neon.cc` + CMake 注册 `vixl_port_neon`。

### Task 3.2：生成 NEON fixture 并转绿

- [ ] **Step 1**：工具 include `test-assembler-neon-aarch64.cc`，构建（NEON 测试可能更多用 `SETUP_WITH_FEATURES(kNEON,...)` 与模板 helper，注意 capture 头是否吞得下；吞不下的用 A3 兜底或 quarantine）。
- [ ] **Step 2**：`vixl_test_extract test/vixl_port/generated/neon_fixtures.inc Neon`。
- [ ] **Step 3**：`ctest -R vixl_port_neon --output-on-failure`，同样分诊。
- [ ] **Step 4**：全绿提交。

**Phase 3 验收**：`vixl_port_neon` 绿；三个 `vixl_port_*` 全部进 CTest 且默认构建自包含。

---

## 收尾 Task：整套验收 + 文档

- [ ] **Step 1**：干净构建自检——`rm -rf build/debug && cmake --preset dev-debug && cmake --build build/debug && ctest --test-dir build/debug --output-on-failure`，确认无 `../vixl`、无 extract option 时三个 `vixl_port_*` 全绿。
- [ ] **Step 2**：在 `docs/testing.md` 增一节，说明 `vixl_port_*` 的来源、刷新方式（开 `GABY_VM_BUILD_VIXL_EXTRACT` + `VIXL_SRC_DIR` 重跑工具）、以及 manifest 的含义。
- [ ] **Step 3**：注入缺陷回归——临时在 cache 轨制造一处偏差（参照 `shadow_runner_test.cc` 的注入手法），确认 `vixl_port_*` 会红，证明 guard rail 对即将到来的 dispatch 优化真的有效；确认后回退。
- [ ] **Step 4**：提交文档更新。

---

## 自查（针对设计 spec 的覆盖核对）

- **覆盖范围（设计 §2 广覆盖）**：Phase 1/2/3 分别覆盖 test-assembler-aarch64/-fp/-neon → ✓。
- **取字节方式（编写期工具）**：Task 0.4 + 1.1 的 capture 宏 + `#include` 真 VIXL .cc → ✓，Tier-0 仅在 `GABY_VM_BUILD_VIXL_EXTRACT` 下出现 → ✓。
- **双 oracle（设计 §6）**：`vixl_port_runner.cc` 的 `DifferentialEqual` + `CheckAssert` → ✓；地址重定位由「差分免疫 + 绝对只对地址无关断言 + writer 跳过地址型断言」覆盖（设计 §6.1）→ ✓。
- **过滤（设计 §7）**：extract_main 的 `kDenyFeatureSubstrings` + 按名 quarantine + manifest 显式列出 → ✓；不静默截断 → ✓。
- **构建接线（设计 §8）**：顶层 option + 工具子目录 + 三个自包含回放测试 → ✓。
- **刻意不做（设计 §10）**：无 SVE、无 trace-matrix、无运行期汇编器、不改 leaf 语义 → 计划未引入 → ✓。
- **占位符扫描**：Phase 0 全代码完整；Record* 系列与 fixture_writer/FP/NEON 细节在对应 Task 给了「按真实 API 对齐」的明确动作而非含糊「TODO」，且都绑定到可验证的 ctest 步骤 → 可接受（这些是依赖现场源码确认的点，已显式标注确认动作）。
- **类型一致性**：`PortedFixture/AssertTarget/AssertKind`、`RunFixture/RunAll/RunStats`、`CaptureRig/HarvestRun/Record*`、生成函数 `<Prefix>Fixtures(size_t*)` 在各 Task 间命名一致 → ✓。
