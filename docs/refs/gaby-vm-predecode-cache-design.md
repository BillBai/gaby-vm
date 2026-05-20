# Gaby-VM Predecode Cache 设计

> 这是 Gaby-VM 预解码缓存的具体设计文档。读者预期是要在 gaby-vm 里实现或 review
> 这个 cache 的人。
>
> **它是什么**：把 `gaby-vm-modification-sketch.md` 里"predecode → cache → execute"
> 那段方向落地成可实施设计——cache 形状、API 形状、哪个 file 改哪一行、哪些 risk
> 怎么消解。
>
> **它不是什么**：不是 OpenSpec change，不是 spec delta，不是 implementation。
> 跟它同 tier 的现有 doc 是 [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
> 和 [`vixl-fetch-decode-dispatch-deep-dive.md`](./vixl-fetch-decode-dispatch-deep-dive.md)。
>
> 所有源码引用用 `path:line` 写法，路径相对仓库根。

## 1. Goal

把 VIXL 上游每条指令 ~300-500 cycle 的 dispatch 链：

```
ExecuteInstruction
  → Decoder::Decode(pc_)
  → CompiledDecodeNode 树游走 (~3-4 层 member-fn-ptr)
  → Decoder::VisitNamedInstruction (Metadata heap-alloc + 第一次 hash)
  → 遍历 visitors_ list (~2-3 个虚函数)
  → Simulator::Visit (form_hash 第二次 hash + unordered_map 查表)
  → leaf 函数（真实工作 ~5%）
```

砍成：

```
ExecuteInstructionCached
  → cache->LookupEntry(pc_)         一次 array load
  → entry.thunk(this, pc_)          一次间接调用
        → form_hash_ = K             编译期立即数 mov
        → leaf(this, pc_)            tail-call
```

leaf 语义本身不动。预解码 pass 在 `RegisterCodeRange` 时跑一次，把每条指令位置上的
form_hash 和 leaf 函数指针烧进一个 8 字节 thunk 函数里，hot path 只剩"查表 + 调 thunk"。

iOS 是主目标——cache 是普通 data，不涉及 codegen / RWX。

## 2. 设计约束

### 2.1 沿用上游设计的硬约束

这些来自 `gaby-vm-modification-sketch.md` 和 `docs/architecture.md`，cache 设计必须
活在它们里：

- **单地址空间**：guest PC 直接是 host pointer，没有 MMU/TLB。cache lookup 拿 PC
  做地址算术，不需要任何 translation。
- **No SMC**：embedder 保证已注册的代码内存在 simulator 生命周期内不变。cache 不做
  invalidation、不做 coherence 检查。
- **No JIT / RWX**：predecoded data 是普通 `const` POD 数据，不是可执行内存。
- **多 Simulator 实例并发**：embedder 一个 host 线程一个 Simulator 实例。每个实例
  独立持有寄存器/PC/NZCV/FPCR/BType/exclusive monitor/stack；shared 的只有"只读"
  的 cache 数据和 form→fn map。
- **Embedder 提供 stack buffer**：不在这次 cache 设计的范围内，但跟构造 API 形状
  相关。

### 2.2 这次新加的硬约束（决议）

- **Cache shared across Simulators**：一个 `gaby_vm::PredecodeCache` 给所有
  Simulator 实例共享。读路径无锁。
- **Cache append-only**：`RegisterCodeRange` 可在 cache 生命周期内反复调用（加载新
  补丁），但已注册的 range 直到 cache 析构都不卸载。V1 没有 `FlushCodeRange`。
- **重叠注册一律 error**：embedder 自己负责 dedup。
- **API 双轨制**：`Simulator::RunFrom` 走 cache 快路径、不支持 trace/debugger/custom
  visitor；`Simulator::DebugRunFrom` 走 VIXL 完整 decoder + visitor 链、慢但功能全。
  两条循环互不干扰，没有中途切换。
- **`Simulator(nullptr, ...)` 合法**：构造时允许不传 cache，那种状态下只能调
  DebugRunFrom。给 ShadowRunner 用，给 bring-up 排查用。
- **ShadowRunner 在 V1 必须存在**：API 式独立模块（不是编译期 flag），lockstep
  对比 cache 路径和 decoder 路径。性能验收 soft，正确性兜底 hard。
- **软性能验收**：bench/baseline + bench/smoke 上有有意义提升 + simulator_correctness
  跨双路径全绿 + ShadowRunner 零 divergence。**不定硬性 N× 数字**。

### 2.3 与既有 doc 的 supersession

`gaby-vm-modification-sketch.md` 里有两段话被这次设计翻转：

| 既有 doc 的说法 | 本设计 | 理由 |
|---|---|---|
| "Per-instance cache（共享是 implementation choice）"（modification-sketch §5 注册时） | Shared cache 是必选 | iOS 补丁包代码量级 100MB+；per-instance 把 cache 体积乘以并发线程数，内存放大不可接受 |
| "Trace 模式 fall through 到 legacy slow path"（modification-sketch §6 Trace fidelity） | 干脆做成 RunFrom + DebugRunFrom 两条 API | hot path 上一个 `predecode_cache_active_` bool 看起来便宜，但带来"切换瞬间状态一致性"复杂度（form_hash_、last_instr_、MOVPRFX chain）。把动态切换提到 API 层，状态一致性问题从根上消失 |

`openspec/specs/aarch64-simulator/spec.md` 里"imported file 必须 byte-identical
except marker regions"那条目前还没改——这是另一个独立的 spec change 范围。本设计
依赖 boundary 已放宽到"修改加 marker 注释说明 why 即可"（HANDOFF §5），实施前
spec 那条要先同步收紧/放宽。

## 3. 不变的东西

cache 替换的是 dispatch 基础设施，不是语义。下列上游机制原样保留：

- **所有 leaf 函数语义**：`Simulator::VisitXxx` 和 `Simulate_*` 系列（含 NEON/SVE
  helpers `LogicAArch64`）一行不动。
- **Simulator state shape**：`registers_[32]`、`vregisters_[32]`、`pregisters_[16]`、
  NZCV、FPCR、FPSR、BType、PC——每个 Simulator 实例一份。
- **Memory class**：`Memory` 类直接 host pointer 模型 (`simulator-aarch64.h:371-490`
  附近)。原子和 barrier 的语义改造按 modification-sketch 走，不在 cache 设计范围。
- **`ExecuteInstruction` 的前后置步骤**：`pc_modified_ = false`、BType 检查、guarded
  page 检查、`last_instr_ = ReadPc()`、`IncrementPc`、`LogAllWrittenRegisters`、
  `UpdateBType` 全部保留。区别只在中间那一段——上游是 `decoder_->Decode(pc_)`，
  cache 路径是 `entry.thunk(this, pc_)`。
- **CPUFeaturesAuditor 类本身**（`cpu-features-auditor-aarch64.h:51-108`）：cache
  populate 阶段会用它，运行期不再用。
- **Bridge to native (`RegisterBranchInterception`)** (`simulator-aarch64.h:3209-3212`)：
  在 leaf 内拦截，cache hot path 不感知。bridge 返回时 LR 必须落在已注册 range，
  否则 abort（见 §4.3）。
- **Decoder 类本身**：cache 持一份给 populate 用；每个 Simulator 还各持一份给
  DebugRunFrom 用。

## 4. 改变了什么

按"形状 / 数据结构 / 边界 / Shadow"分四块说。每块都附**为什么是这个选择**——后来
的人需要这个，不只是结论。

### 4.1 API 双轨制（最大的形状决定）

```cpp
namespace gaby_vm {
class Simulator {
 public:
  // cache 可为 nullptr —— 那种状态下只能调 DebugRunFrom
  Simulator(PredecodeCache* cache, void* stack_buf, size_t stack_size);
  ~Simulator();

  // Fast path. 走 predecode cache。要求 cache != nullptr。
  // 不支持 trace/debugger/custom visitor。
  // PC 必须始终落在 cache 已注册的 range 内，否则 abort。
  void RunFrom(uintptr_t entry_pc);

  // Slow path. 走 VIXL 完整 decoder + visitor 链。可观测性完整。
  // 性能 ~50× 慢。给诊断 / 调试 / ShadowRunner 用。
  void DebugRunFrom(uintptr_t entry_pc);

  // Lockstep 单步原语。给 ShadowRunner 和自定义诊断循环用。
  bool StepOnce();         // 走 cache 路径；要求 cache != nullptr
  bool DebugStepOnce();    // 走 decoder 路径；永远可用

  // Trace / visitor 设置只在 DebugRunFrom 模式下生效；
  // RunFrom 模式下被静默忽略（embedder 责任）。
  void SetTraceParameters(int flags);
  void AppendDecoderVisitor(/* ... */);

  // 寄存器读写、PC、CPU features 等共用。
  void WriteRegister(int idx, uint64_t value);
  uint64_t ReadRegister(int idx) const;
  // ...
};
}  // namespace gaby_vm
```

**为什么不是单一 Run + 内部 bool gate**：bool gate 的实现思路是"hot path 一次 bool
读 + 分支预测器吃掉代价"。这个本身不贵，问题在切换瞬间的状态一致性。cache 路径写
`form_hash_` 但不调 `Decoder::Decode`，所以 decoder 内部状态（如
`last_instr_`、MOVPRFX chain 计数）不更新；fallback 路径走完整 decoder，会更新
这些。如果允许 hot path 上动态切换，就要在切换瞬间补一组 reset 逻辑，否则连续两条
指令前一条走 cache、后一条走 fallback 时，fallback 那条看到的 `last_instr_` 是过时
的——deep-dive R2 提到的 MOVPRFX chain 检查直接出 bug。

把切换提到 API 层，hot path 内**永远是同一条循环**，状态一致性问题从根上不存在。
代价只是 API 表面多一个方法——这是个非常好的取舍。

**为什么 trace 设置在 RunFrom 下静默忽略而不是 assert**：embedder 知道自己在干啥；
用 RunFrom 就是承诺"我不要 trace"。assert 的话相当于强制 embedder 在切换 Run 模式
之前手动 reset trace 设置，添麻烦没收益。

**为什么允许 nullptr cache**：开了一个"纯 VIXL 行为"的回退场景。两个用途：
1. ShadowRunner 内部需要一个不走 cache 的 Simulator 作 oracle reference——直接用
   `Simulator(nullptr, ...) + DebugRunFrom` 就行，不用为 oracle 单独造类。
2. Bring-up 阶段如果 cache 实现疑似有 bug，embedder 可以零代码改动切到纯 decoder
   路径，把"cache 实现 bug"和"VIXL leaf bug"两类问题分开。

### 4.2 数据结构

#### 4.2.1 PredecodedEntry：8 字节 + per-form thunk

```cpp
struct PredecodedEntry {
  void (*thunk)(Simulator*, const Instruction*);   // 8 bytes
};
```

每个 form 一个专属 thunk，由 C++ template 自动生成：

```cpp
template <auto MemberFn, uint32_t FormHashConstant>
void PredecodeThunk(Simulator* sim, const Instruction* instr) {
  sim->form_hash_ = FormHashConstant;          // 编译期立即数 → mov immediate
  (sim->*MemberFn)(instr);                      // tail-call 真 leaf
}
```

Cache populate 阶段，每个 PC 位置根据 form_hash 选对应的 `PredecodeThunk` 实例化，
把它的函数指针写进 entry。

**为什么不是 16 字节直存 `{form_hash, leaf_fn}`**：

1. **cache 体积减半**：100MB 代码 = 25M 指令 × 8 bytes = 200MB cache（thunk 方案）
   vs 400MB（直存）。在 iOS 大补丁包场景体积差距不可忽略。
2. **form_hash 写从 load+store 变 mov+store**：thunk 体里 `form_hash_ = K` 是编译期
   常量，编译器直出 `mov w_reg, #K; str w_reg, [...]` 两条 ARM 指令。直存方案需要
   `ldr w_reg, [entry, #form_hash_offset]; str w_reg, [...]`——多一次 load。
3. **V2 留 hook 点**：thunk 体未来可以加 operand pre-extraction（thunk 体内提前
   `Rd = bits[4:0]; imm12 = bits[21:10]` 之类），这条优化路径在直存方案里没地方插。

代价是 ~2300 个 thunk × ~24 bytes (mov+str+b 三条 ARM 指令的近似) ≈ 55-70KB rodata
binary 增量。可忽略。

#### 4.2.2 CodeRange + 范围表

```cpp
struct CodeRange {
  const Instruction* start;     // host 指针
  size_t             size_bytes;
  PredecodedEntry*   entries;   // size_bytes / 4 个 entry
};
```

cache 内部维护一张 `std::vector<CodeRange>`，按 `start` 排序。append-only：
- 写者（`RegisterCodeRange`）拿一把 mutex 串行追加。
- 读者无锁，acquire-load 看 tail 指针。

**append-only 是并发简化的关键**：entry 数组永不重定位（grow 不会 realloc 已分配的
range entry），意味着热路径里缓存的 `cur_range_` 指针在 cache 生命周期内永不失效。
不需要 generation counter、不需要 hazard pointer。这是 V1 选 append-only 而不选支持
Flush 的根本原因——支持 Flush 就要重新设计读者无锁路径，复杂度上一个台阶。

#### 4.2.3 PC → entry 查找：cur_range_ cache + 二分

每个 Simulator 实例（不是 cache 自己——这点重要，见下）持一个 `cur_range_` 指针。
hot path 形状：

```cpp
inline PredecodedEntry* Simulator::LookupEntry(const Instruction* pc) {
  if (VIXL_LIKELY(cur_range_)) {
    uintptr_t off = uintptr_t(pc) - uintptr_t(cur_range_->start);
    if (VIXL_LIKELY(off < cur_range_->size_bytes)) {
      return &cur_range_->entries[off >> 2];
    }
  }
  return cache_->LookupEntrySlow(pc, &cur_range_);   // 二分 + 更新 cur_range_
}
```

**为什么 `cur_range_` 在 Simulator 而不在 Cache**：cache 是 shared、read-only。
`cur_range_` 是个 mutable per-execution state，跟 PC 一起属于 Simulator 实例的本地
状态。放 Simulator 上每个实例一份，无 race。

**slow path 数据结构选 sorted array + 二分**：range 数量预期 10²~10³（一个 iOS 大
补丁包大概几十到几百个 dylib，每个 dylib 一个 range）。log₂(1024) = 10 hop，cold
path 完全够。radix tree、page-bucketed hash 之类有更好的渐进复杂度但常数更大、
实现复杂得多——cur_range_ 命中率 99%+ 的情况下没必要。

### 4.3 边界情况

#### 4.3.1 Out-of-range PC

`RunFrom` 路径下 PC 跳到没注册的地址 → `VIXL_ABORT_WITH_MSG("PC <addr> not in any
registered code range")`。

embedder 怀疑某段没注册全 → 自己切到 `DebugRunFrom`（VIXL 原生 decoder，所有有效
AArch64 编码都能跑）作诊断。

**为什么不做 fallback**：fallback 等于在 hot path 上又加一个 bool gate（cache miss
后是 abort 还是降级），把 §4.1 那个简化又退回去。embedder 用 RunFrom 就是承诺"PC
不会跑出去"——跑出去了是 embedder bug，hard fail 比悄悄变慢更容易诊断。

#### 4.3.2 RegisterCodeRange 错误返回

```cpp
enum class RegisterStatus {
  Ok,
  UnsupportedFeature,    // CPUFeaturesAuditor 拒绝某条指令
  OverlappingRange,      // 跟已注册的 range 有任何形式重叠
  OutOfMemory,           // 分配 entry 数组失败
  InvalidArgument,       // size 不是 4 的倍数等
};

class PredecodeCache {
 public:
  RegisterStatus RegisterCodeRange(const void* start, size_t size_bytes);

  // 拿到上一次 RegisterCodeRange 失败的具体细节。
  // 跨线程不安全；调用者负责确保跟 RegisterCodeRange 在同一线程。
  struct ErrorDetail {
    uintptr_t      pc;             // 出错的 PC（UnsupportedFeature 时有效）
    const char*    reason;         // 静态字符串
    CPUFeatures    missing;        // 缺失的 CPU features（UnsupportedFeature 时有效）
  };
  const ErrorDetail* GetLastErrorDetail() const;
};
```

**all-or-nothing 而不是部分成功**：iOS 补丁场景下，embedder 拿到 dylib 可能事先不
完全清楚里面用了什么 CPU features。报错应该让 embedder 知道**哪条 PC 用了什么
feature**，让它能做"补 feature 后重试"或者"放弃这个 dylib"的决策。部分成功（已注册
的 entry 标记为 `unsupported_thunk`）让 cache 维护一组半成品状态，复杂度收益不
划算。

**为什么 errno+strerror 风格而不是把细节塞进返回码**：错误细节天然多维度（PC、
form name、feature mask）。返回码保持简单稳定（C-ABI 友好），细节通过单独 query
拿。将来加新错误类型不破坏 API。

#### 4.3.3 重叠注册

任何形式的地址区间重叠（含完全相同）→ `OverlappingRange`。embedder 自己负责 dedup。

**为什么这么严**：cache 是 append-only，"覆盖"语义无法低成本实现（要原地改 entry，
跟 append-only 冲突）。允许 noop（完全相同地址→静默成功）听起来友好，但隐藏 bug：
embedder 写错代码两次注册同一段，希望第二次更新；如果两次内容一致就 noop、不一致
就 error，行为不一致。统一拒绝最 KISS。

#### 4.3.4 内存预算

embedder 自己负责。N 字节代码 = 2N 字节 cache（4 字节指令 → 8 字节 thunk pointer）。
cache 不做 cap、不做 LRU、不做 Flush。

iOS 100MB 大补丁包 → 200MB cache。embedder 知道自己在干啥。

### 4.4 Shadow self-test：ShadowRunner

V1 必须有的正确性兜底。**不是**编译期 flag，是个 always-built 的 API 模块。

```cpp
namespace gaby_vm::testing {

struct DivergenceReport {
  uintptr_t                 pc;
  enum class Kind { Register, MemoryWrite, ExecutionState } kind;

  // Register divergence
  int                       reg_index;       // -1 if not applicable
  uint64_t                  reg_fast;
  uint64_t                  reg_ref;
  const char*               reg_name;        // "X3", "V7.D[1]", "NZCV", ...

  // Memory write divergence
  uintptr_t                 mem_addr;        // 0 if not applicable
  size_t                    mem_size;
  uint64_t                  mem_fast_lo, mem_fast_hi;
  uint64_t                  mem_ref_lo,  mem_ref_hi;
};

class ShadowRunner {
 public:
  // 内部持两个 Simulator：
  //   fast_  = Simulator(cache,   stack_buf, ...) → RunFrom 路径
  //   ref_   = Simulator(nullptr, stack_buf, ...) → DebugRunFrom 路径
  // 共享 stack_buf（同一段内存），保证寄存器对比 byte-equal。
  ShadowRunner(PredecodeCache* cache, void* stack_buf, size_t stack_size);
  ~ShadowRunner();

  // 镜像写两个 Simulator 的初始寄存器
  void WriteRegister(int idx, uint64_t value);
  uint64_t ReadRegister(int idx) const;

  // Lockstep 跑：交替单步 fast_ 和 ref_，每步对比；divergence 触发 handler。
  void RunFrom(uintptr_t entry_pc);

  // 默认 handler 是 dump diff 到 stderr + abort。
  // 注入 callback 可改成 non-fatal（test harness 用）。
  using DivergenceHandler = std::function<void(const DivergenceReport&)>;
  void SetDivergenceHandler(DivergenceHandler h);
};

}  // namespace gaby_vm::testing
```

**为什么 lockstep 而不是各跑各的最后比终态**：bug 通常在第一次 divergence 之后会被
后续指令掩盖或放大。lockstep 能精确指出"PC=0x... 这条指令两条路径分叉了"——配
DivergenceReport 直接定位 form 级别的实现 bug。

**为什么共享 stack buffer**：寄存器（特别是 SP、X29 frame pointer）值要 byte-equal
才能直接对比。两 Simulator 初始 SP 必须相等 → 必须指向同一块内存 → 共享 buffer。
后果是两边写同一地址，后写的覆盖先写的——但有 §4.4.2 的 memory write trace 兜底，
这个掩盖效应不会让 bug 漏检。

#### 4.4.1 状态对比范围

V1 拿这一组：

| 类别 | 字段 |
|---|---|
| 通用寄存器 | X0-X30, SP |
| FP/SIMD | V0-V31（128-bit 对比） |
| PC | pc_ |
| 标志位 | NZCV, FPCR, FPSR |
| 控制状态 | BType |
| 内存写 | per-step write trace（见 §4.4.2） |

**V1 不入 V2 处理**：SVE Z 寄存器、SVE 谓词 P0-P15、SVE FFR、排他监视器内部状态。
理由：iOS 补丁包大概率不用 SVE（A12 之前的 iPhone 就不支持），排他监视器的 bug 会
通过 STXR 成功/失败位反映到通用寄存器，间接被覆盖。

#### 4.4.2 内存写 trace（关键的 imported file 修改）

每步开始时清空 trace buffer；fast 和 ref 各自跑过一条指令后，对比两个 buffer 的
write list（addr, size, value）；不一致 → divergence。

实现需要在 imported VIXL `Simulator` 类的内存写路径加 hook：

```cpp
// src/aarch64/simulator-aarch64.h (imported VIXL，加 marker 注释式修改)

// gaby-vm: ShadowRunner 用这个 sink 抓每步内存写，做双路径 byte-level 对比。
//          默认 nullptr，hot path 多一次 likely-not-taken branch ≈ 0 cycle。
//          详见 docs/refs/gaby-vm-predecode-cache-design.md §4.4.2 + §5.7。
class MemoryWriteSink {
 public:
  virtual ~MemoryWriteSink() = default;
  virtual void Record(uintptr_t addr, size_t size,
                      uint64_t value_lo, uint64_t value_hi) = 0;
};

class Simulator {
  // ... existing fields ...

  // gaby-vm: optional per-step memory write observer
  MemoryWriteSink* write_sink_ = nullptr;

 public:
  void SetMemoryWriteSink(MemoryWriteSink* sink) { write_sink_ = sink; }

  template <typename T>
  void MemWrite(uintptr_t addr, T value) {
    // 上游 memcpy / atomic 逻辑保留不动。

    // gaby-vm: shadow hook
    if (VIXL_UNLIKELY(write_sink_)) {
      write_sink_->Record(addr, sizeof(T),
                          static_cast<uint64_t>(value), 0);
    }
  }
};
```

128-bit SIMD 写（`MemWrite<vixl::aarch64::qreg_t>` 或类似）需要分高低 64 位记录到
`value_lo` / `value_hi`，但都走同一 hook。

**为什么不是 dual buffer + 终态 memcmp**：dual buffer 方案两边的 stack 在不同地址，
寄存器里的指针值不等，简单的 byte-equal 比较就不成立——要做 base-relative 转换，
复杂度比 sink hook 还高。sink hook 把"内存写发生时谁写了什么"明确化，是更可观察、
更精确的 oracle。

#### 4.4.3 测试集成

- `test/simulator_correctness.cc` 改造成参数化跑 RunFrom + DebugRunFrom 双路径。
- 新增 `test/shadow_runner_test.cc`：覆盖 ShadowRunner 自身正确性 + DivergenceHandler
  callback 流程 + 故意注入 fast_ leaf bug 验证 divergence detection。
- `bench/baseline.cc`、`bench/smoke.cc` 引入 cache-enabled 变体（在现有 baseline
  之外）。**ShadowRunner 不进 bench 默认 build**——它会让每条指令变慢 ~50×，bench 数
  据无意义。

### 4.5 性能验收（软）

V1 验收三条：

1. `bench/baseline` 和 `bench/smoke` 上 dispatch overhead **有有意义的提升**——具体
   数字以实测为准，期望"几倍量级"（deep-dive 推的 300→10 cycle 是估算反推，未必贴
   实际）。
2. `simulator_correctness` 跨 RunFrom + DebugRunFrom 双路径**全绿**。
3. ShadowRunner 跑 `bench/workloads/*` 上现有 workloads **零 divergence**。

**为什么不定硬性 N× 数字**：硬数字会诱导为指标做不必要的优化（thunk 过度内联、
强行 vector 化 lookup 等），偏离"correctness first, performance second"的项目原则。
软验收 + ShadowRunner 强 oracle 是更健康的组合——后者保证不回归、前者保证有进展，
具体多少由实测说话。

## 5. 新结构

按文件分组列。每个新结构标记**职责**和**关键依赖**，方便实现阶段拆 OpenSpec change。

### 5.1 `gaby_vm::PredecodeCache`

**位置**：`include/gaby_vm/predecode_cache.h`（API） + `src/gaby_vm/predecode_cache.cc`
（impl，Pimpl）。

**职责**：管理 cache 生命周期、注册 code range、跑 predecode pass、暴露 lookup 给
Simulator。

**关键依赖**：
- 持有一个 baseline-only `vixl::aarch64::Decoder`（给 populate 用）。
- 持有一个 `vixl::aarch64::CPUFeaturesAuditor`（给 populate 用，运行期不再用）。
- 持有 `std::vector<CodeRange>` + 写者 mutex。
- 通过 `Simulator::GetFormToVisitorFnMap()` 拿到 form_hash → leaf_fn 表（这点决定
  了 imported simulator-aarch64.cc 要不要加 marker 暴露——见 §6.2）。

### 5.2 `gaby_vm::PredecodedEntry` + `gaby_vm::detail::PredecodeThunk<>`

**位置**：`include/gaby_vm/predecode_cache.h`（PredecodedEntry，给 Simulator hot
path inline lookup 用）+ `src/gaby_vm/thunks.h`（PredecodeThunk template + 实例化
工具）+ `src/gaby_vm/thunk_table.cc`（~2300 个实例化清单，可由脚本从 visitor map
生成）。

**职责**：见 §4.2.1。

**实现注意**：thunk 是 `void (*)(Simulator*, const Instruction*)` 函数指针。Simulator
class 是 `vixl::aarch64::Simulator`，对 thunk 来说是 imported 类型——thunk 如果直接
访问 `sim->form_hash_` 字段，需要 friend 或者把 form_hash_ 改 public（加 marker）。
具体形式留给实施。

### 5.3 `gaby_vm::CodeRange`

**位置**：`include/gaby_vm/predecode_cache.h`。

**职责**：见 §4.2.2。

**接口形态**（embedder 看到的）：

```cpp
RegisterStatus RegisterCodeRange(const void* start, size_t size_bytes);
```

`CodeRange` 本身不直接出现在 public API——embedder 提供 `(start, size)` 给注册函数，
cache 内部构造 CodeRange。

### 5.4 `gaby_vm::RegisterStatus` + `gaby_vm::ErrorDetail`

**位置**：`include/gaby_vm/predecode_cache.h`。

**职责**：见 §4.3.2。

**ABI 友好**：`enum class : int`（C-ABI 兼容），`ErrorDetail` 是 POD struct
（无 std::string，用 `const char*` 静态字符串）。

### 5.5 `gaby_vm::Simulator`

**位置**：`include/gaby_vm/simulator.h`（API，Pimpl 包装） + `src/gaby_vm/simulator.cc`
（Pimpl 胶水）。

**职责**：见 §4.1。Pimpl 隔离的目的是让 embedder 头文件看不到 `vixl::*`——符合现有
`include/gaby_vm/` 习惯（gaby_vm.h 也是干净 facade）。

**Impl 关系**：`Simulator::Impl` 内部持有一个 `vixl::aarch64::Simulator` + 一个
`vixl::aarch64::Decoder`（给 DebugRunFrom 用）+ 一个 `PredecodeCache*` 指针（可
nullptr）。

### 5.6 `gaby_vm::testing::ShadowRunner` + `DivergenceReport` + `DivergenceHandler`

**位置**：`include/gaby_vm/shadow_runner.h` + `src/gaby_vm/shadow_runner.cc`。
namespace `gaby_vm::testing`——暗示给 test/diagnosis 用，不是生产 hot path。

**职责**：见 §4.4。

**实现关系**：内部持两个 `gaby_vm::Simulator`，按 lockstep 调它们的 `StepOnce` /
`DebugStepOnce`。每步开始 `SetMemoryWriteSink(&fast_sink_)` / `SetMemoryWriteSink
(&ref_sink_)`，结束后对比 sink 内容。

### 5.7 `vixl::aarch64::MemoryWriteSink` + `Simulator::write_sink_`

**位置**：`src/aarch64/simulator-aarch64.h`（imported VIXL，加 marker 注释式修改）。

**职责**：见 §4.4.2。

**为什么放 vixl::aarch64 namespace 而不是 gaby_vm**：sink 接口跟 imported Simulator
class 紧耦合（hook 点在 `Simulator::MemWrite` template 里）。放 gaby_vm namespace
就需要 `vixl::aarch64::Simulator` include `gaby_vm/...`，反向依赖，不干净。放
vixl::aarch64 内更自然——可以视为 gaby-vm 在 imported VIXL 上加的一个 trait class。

## 6. 插入点（file:line 锚点）

按文件分。**每处修改都要带 marker 注释解释 why**，参照 §6.5 的样例。

### 6.1 `src/aarch64/simulator-aarch64.h`

| 锚点 | 修改内容 |
|---|---|
| 类前部（合适位置） | 新增 `class MemoryWriteSink`（见 §5.7） |
| 类成员变量区 | 加 `MemoryWriteSink* write_sink_ = nullptr;` |
| 类公开方法区 | 加 `void SetMemoryWriteSink(MemoryWriteSink*)` |
| ~`h:1401-1442` `ExecuteInstruction` 附近 | 新增 `ExecuteInstructionCached`（cache 路径专用，沿用 BType / IncrementPc / UpdateBType 但跳过 auditor VIXL_CHECK） |
| ~`h:1401-1442` `ExecuteInstruction` 内 | 上游 ExecuteInstruction 不动，给 DebugRunFrom 用 |
| 新增（公开方法） | `bool StepOnce()` / `bool DebugStepOnce()` |
| `MemWrite<T>` template body 末尾 | 加 `if (VIXL_UNLIKELY(write_sink_)) write_sink_->Record(...)` |

`form_hash_` 字段（thunk 要写）当前可见性需查一下 imported 源；如果是 private 要么
开 `friend gaby_vm::detail::ThunkAccess`，要么改 public 加 marker。

### 6.2 `src/aarch64/simulator-aarch64.cc`

| 锚点 | 修改内容 |
|---|---|
| `cc:105-124` `GetFormToVisitorFnMap()` | 可能需要 friend `gaby_vm::PredecodeCache` 让它能拿到这张表；或者 cache 自己用 `Decoder + PredecodeVisitor` 间接获取 |
| `cc:2314-2323` `Simulator::Visit` | 不动——这条路径只在 DebugRunFrom 时用 |
| `cc:840` `RunFrom` 等 | 加新方法或委托 |

### 6.3 `include/gaby_vm/`

全新文件：

| 文件 | 内容 |
|---|---|
| `gaby_vm.h` | 已有 facade；可能加 `#include "gaby_vm/simulator.h"` 等 |
| `predecode_cache.h` | `class PredecodeCache`、`RegisterStatus`、`ErrorDetail` |
| `simulator.h` | `class Simulator`（Pimpl） |
| `shadow_runner.h` | `namespace testing { class ShadowRunner; struct DivergenceReport; }` |

### 6.4 `src/gaby_vm/`（全新目录）

| 文件 | 内容 |
|---|---|
| `predecode_cache.cc` | `PredecodeCache::Impl`、populate pass、range 表管理 |
| `simulator.cc` | Pimpl 胶水、Run / DebugRun / StepOnce 实现 |
| `shadow_runner.cc` | lockstep 逻辑、状态对比、diff dump |
| `thunks.h` | `PredecodeThunk` template + ThunkAccess friend trait |
| `thunk_table.cc` | ~2300 个 thunk 实例化清单（脚本生成） |

### 6.5 marker 注释风格（首处事实标杆）

`src/aarch64/simulator-aarch64.{h,cc}` 当前**没有任何 marker**——是干净的 baseline
import。这次加的第一处 marker 等于建立事实标杆，后面所有修改都会模仿这一处。

样式约定：

- **用中文**（按 CLAUDE.md "中文表达风格"那条）。
- **解释 why，不只是 what**。"加了 sink 字段"是 what；"why 是 ShadowRunner 需要每步
  trace、why 默认 nullptr 是不要影响 hot path"是 why。
- **引设计文档**。用相对路径 `docs/refs/gaby-vm-predecode-cache-design.md §X.Y`。
- 单行修改用 `// gaby-vm: ...`；多行块用 `// gaby-vm BEGIN ... // gaby-vm END`。

具体样例：

```cpp
// gaby-vm: ShadowRunner 用这个 sink 抓每步内存写，做双路径 byte-level 对比。
//          默认 nullptr，hot path 多一次 likely-not-taken branch ≈ 0 cycle。
//          详见 docs/refs/gaby-vm-predecode-cache-design.md §4.4.2 + §5.7。
MemoryWriteSink* write_sink_ = nullptr;
```

```cpp
// gaby-vm BEGIN: 新增 cache 命中路径。原 ExecuteInstruction 给 DebugRunFrom 用，
//                两条独立循环，无中途切换——避免 form_hash_ / last_instr_ 状态错位。
//                详见 docs/refs/gaby-vm-predecode-cache-design.md §4.1。
void Simulator::ExecuteInstructionCached() {
  // ... pre-checks 同 ExecuteInstruction ...
  PredecodedEntry* e = cache_->LookupEntry(pc_);
  if (VIXL_UNLIKELY(!e)) {
    VIXL_ABORT_WITH_MSG("PC not in any registered code range");
  }
  e->thunk(this, pc_);
  // ... post-checks 同 ExecuteInstruction，但跳过 auditor VIXL_CHECK
  //     auditor 已在 RegisterCodeRange 时审过 ...
}
// gaby-vm END
```

## 7. 风险——重审 deep-dive R1-R12

deep-dive `vixl-fetch-decode-dispatch-deep-dive.md` §5 列了 R1-R12 共 12 条 risk。
逐条说明 V1 怎么处理。

| # | Risk | V1 处理 |
|---|---|---|
| **R1** | `form_hash_` 必须在 leaf 调用前正确 | thunk 体内 `sim->form_hash_ = K`（编译期立即数）→ leaf 调用，永远先写后调。无切换路径，无 race |
| **R2** | MOVPRFX chain 要 `last_instr_` + `form_hash_` 跨 cache miss 也正确 | API 双轨制 → cache 路径全程不 fallback，`last_instr_` 由 cache 路径自己更新（thunk 后置由调用端在 `ExecuteInstructionCached` 末尾 `last_instr_ = ReadPc()`，跟 ExecuteInstruction 一致）。无混跑路径 |
| **R3** | CPUFeaturesAuditor 怎么处理 | populate 阶段全部预审；运行期 hot path 不再 audit。RegisterCodeRange 失败返回 `UnsupportedFeature` + ErrorDetail（§4.3.2） |
| **R4** | Trace/debugger/custom visitor fallback | API 双轨制 → 这些功能挂 DebugRunFrom 路径；RunFrom 路径里这些 setter 静默忽略 |
| **R5** | BType / guarded page runtime state | `ExecuteInstructionCached` 前置部分跟 ExecuteInstruction 完全一致（包含 BType / guarded page 检查）。这部分属于"unchanged"（§3） |
| **R6** | SMC | 跟 modification-sketch 一致：embedder 保证 immutable。cache 没有 invalidation API |
| **R7** | 多 Simulator 并发 | shared cache，注册期写者 mutex，运行期读者无锁。append-only 让 entry 数组永不重定位，cur_range_ 不需要任何同步原语 |
| **R8** | Cache 无界增长 | append-only；embedder 自负责。无 cap、无 LRU、无 Flush（§4.3.4） |
| **R9** | iOS no-JIT | thunk 是普通 C++ 函数（编译期静态生成），entry 是普通 data。不涉及 RWX、不涉及 mprotect |
| **R10** | Byte-identical correctness 怎么验证 | **ShadowRunner 是 V1 的核心 oracle**（§4.4）。寄存器 + 内存写每步对比，零 divergence 是 V1 验收第三条。这是 R10 的直接答复 |
| **R11** | 间接分支首次命中无加速 | V1 不优化间接分支预测——`WritePc` 后下一轮 cache lookup 走 cur_range cold path（如果跨 range）。可接受。V2 可加 indirect target 预测器 |
| **R12** | Unallocated/unimplemented 编码 | populate 阶段碰到 unallocated → ErrorDetail 标 PC 后注册失败。unimplemented（VIXL 已知但未实现）：populate 时映射到一个特殊 `unimplemented_thunk`，运行到该 PC 时 abort 并报 form name |

**额外的、deep-dive 没列但本设计要警惕的**：

- **ShadowRunner 自身的正确性**：oracle 比被测对象简单（lockstep + 状态比较），但
  仍要写 `test/shadow_runner_test.cc` 故意注入 fast_ bug，验证 ShadowRunner 真能
  catch divergence。否则 oracle 失效是 silent failure。
- **marker 风格漂移**：第一处 marker 设标杆。后续 PR review 要把 marker 注释当作
  代码本身严格 review，避免几次 PR 之后注释退化成 "// gaby-vm: changed"。
- **GetFormToVisitorFnMap 的访问方式**：cache populate 需要这张表。如果 friend
  `gaby_vm::PredecodeCache`，`vixl::aarch64::Simulator` class 的头文件就要 include
  forward-declare gaby_vm 的类——要小心反向依赖。备选：cache 内部跑一次 Decoder +
  自定义 visitor，在 visitor 里捕获 form_hash → leaf_fn 映射。后者更解耦。

## 8. Non-goals

V1 不做的事，列在这里避免被 scope creep 拽进去：

- **`FlushCodeRange` / cache invalidation**：append-only。
- **Operand pre-extraction**：thunk 体里以后可以加，但 V1 thunk 只写 form_hash + tail-call leaf。
- **Basic-block linking**：thunk 之间互相调用预解码出来的下一条 entry。V2 优化方向。
- **Direct threading**：把"循环 + LookupEntry + thunk call"折叠成 thunk 之间互相 jmp。V2+。
- **SVE Z/P/FFR shadow 对比**：V1 范围只到通用寄存器 + V 寄存器。
- **排他监视器内部状态对比**：通过 STXR 成功/失败位间接覆盖。
- **SMC support**：跟 modification-sketch 一致。
- **硬性性能数字验收**：软验收 + ShadowRunner 强 oracle。
- **per-instance cache fallback**：决议 §2.2 已 supersede。
- **cache memory cap / LRU**：embedder 责任。
- **运行期 cache vs decoder 切换**：API 双轨制把切换提到 API 层。
- **`openspec/specs/aarch64-simulator/spec.md` 里 byte-identical 条款放宽**：另一个
  独立的 spec change 范围。

## 接下来读什么

- [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md) — 总方向，
  本设计是它的具体落地。
- [`vixl-fetch-decode-dispatch-deep-dive.md`](./vixl-fetch-decode-dispatch-deep-dive.md)
  — cycle 拆解 + R1-R12 risk 列表，本设计 §7 直接回应。
- [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md) — file:line
  事实参考，写实施 PR 时查 anchor 用。
- [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md)
  — Simulator 子系统总览，给"什么是 unchanged"建立背景。
- [`vixl-extraction-map.md`](./vixl-extraction-map.md) — 文件 tier 列表，决定哪些
  imported file 可以加 marker、哪些不能动。
- `docs/architecture.md` — 项目级架构，特别是 memory model / threading model /
  VIXL import boundary 三段。
- `AGENTS.md` / `CLAUDE.md` — agent rules，含 VIXL boundary 政策（已放宽）和中文
  表达风格。
