# Predecode / Decode Cache 设计 brainstorm — Handoff

> **状态**：brainstorm 进行到一半，上一个 session 表现不好，user 决定开新 session 继续。
> 这份文档把已达成的共识、待拍板项、还没讨论的开放问题整理出来，下个 session 可以直接接着聊，不用从头重来。
>
> **重要前提**：这**不是** OpenSpec change。是一个普通的设计 brainstorm，最终产出物是
> `docs/refs/` 下面一份设计文档（跟现有的 `gaby-vm-modification-sketch.md` 和
> `vixl-fetch-decode-dispatch-deep-dive.md` 同 tier）。OpenSpec change 是后面真要实现
> 的时候才整。

## 背景

Gaby-VM 主优化方向（CLAUDE.md）：

```
predecode once -> cache decoded dispatch target -> execute cached path repeatedly
```

VIXL 上游每条指令 dispatch 开销 300~500 cycle 量级，其中 leaf 真实工作只占 ~5%。最大头是
`Decoder::VisitNamedInstruction` 里的 Metadata heap 分配 + 双重 string hash + visitor list
迭代，加 `Simulator::Visit` 里 form_hash → leaf_fn 的 `std::unordered_map` 查表。

预解码缓存把这条链整段砍掉，命中路径变成"一次 array load + 一次间接调用"。leaf 语义本身
不动。iOS 是主要目标 → 缓存项是普通 data，不是 codegen。

## 已经确认 / user 明确认可的决定

### 1. V1 范围：最小命中替换

**只缓存 `(form_hash, leaf_fn)`**。把 `ExecuteInstruction` 改成"查表 → 写 `form_hash_`
→ 调 leaf_fn"。
operand pre-extraction、basic-block linking、direct threading 全部留到 V2+。

### 2. Populate 模型：preheat，无 lazy populate，无 invalidate

embedder 在加载补丁包时一次性把所有 Mach-O dylib 的可执行 section 注册并 preheat 完。
后续运行期可能再加载新补丁，但**比较低频**——意味着 RegisterCodeRange 不是 setup-only，
要支持运行期被调用。无 SMC 支持。

### 3. Workload 画像：iOS 补丁包系统

- 一个"补丁包" = N 个 Mach-O dylib
- 多 Simulator 实例（一线程一个），共享同一份 read-only cache
- VM ↔ native 频繁交错。native 调用走 VIXL 现成的 `RegisterBranchInterception`
  （`simulator-aarch64.h:3213`），在 leaf 内拦截，cache hot path 不感知
- 一个进程里 code range 数量量级是几十到几百

### 4. Cache 所有权：独立 `PredecodeCache` 对象

`gaby_vm::PredecodeCache` 是个独立对象，embedder 控制生命周期。一个 cache 给多个
Simulator 实例共享（cache 是注册后只读的，零 race）。Simulator 构造时把 cache 指针挂上。

### 5. VIXL boundary 政策已放宽

不再要求 imported VIXL 文件 byte-identical to upstream。**修改加注释说明原因即可**，方便后面审计/对版。这个是 user 明确说的更新，下次写设计时不再被原 spec 那条
"byte-identical except in marker regions"绑住。`openspec/specs/aarch64-simulator/spec.md`
将来要对应同步收紧/放宽。

## Claude 的推荐答案，user 没明确否决但也没明确点头

下面这些是我（上个 session 的 Claude）一路顺着推的方案。每条都给了 user 讨论机会，user
没反对，但也没用 multi-choice 选项明确拍板。**下个 session 建议每条都简短再确认一次**，
避免我自己 over-commit。

### 6. PC → entry lookup：多 range + cached `cur_range_`

Hot path 伪代码：

```cpp
inline PredecodedEntry* LookupEntry(const Instruction* pc) {
    if (VIXL_LIKELY(cur_range_)) {
        uintptr_t off = uintptr_t(pc) - uintptr_t(cur_range_->start);
        if (VIXL_LIKELY(off < cur_range_->size_bytes)) {
            return &cur_range_->entries[off >> 2];
        }
    }
    return LookupEntrySlow(pc);   // 二分查 range 表 + 更新 cur_range_
}
```

理由：AArch64 顺序执行 + 同一函数内分支大概率不跨 range，cur_range 命中率应该 99%+。
跨 dylib 调用付一次 cold path 后又回归 hot。

### 7. PredecodedEntry 布局：8 bytes + per-form thunk

entry 只装 thunk_fn pointer：

```cpp
struct PredecodedEntry {
    void (*thunk)(Simulator*, const Instruction*);   // 8 bytes
};
```

每个 form 一个专属 thunk，由 C++ template 自动生成：

```cpp
template <auto MemberFn, uint32_t FormHashConstant>
void PredecodeThunk(Simulator* sim, const Instruction* instr) {
    sim->form_hash_ = FormHashConstant;          // 编译期立即数
    (sim->*MemberFn)(instr);                      // tail-call 真 leaf
}
```

理由：

- entry 大小减半（vs 16-byte `{form_hash, leaf_fn}` 直存）
- `form_hash` 的 write 从"load entry 字段 + store"变成"mov immediate + store"，省一次 load
- ~2300 个 thunk × ~24 bytes = ~55-70KB 额外二进制，可忽略
- 给 V2 留了天然 hook 点：thunk 体里以后可以加 operand pre-extraction

**注意**：上一个 session 在解释 thunk 时 user 花了两轮才理解。下次讨论时可以更直接：
"thunk 就是一个中间转发函数，里头把 form_hash 当编译期常量写入 simulator，再调真正的 leaf"。

### 8. Public API：Pimpl 隔离

```cpp
// include/gaby_vm/predecode_cache.h
namespace gaby_vm {
class PredecodeCache {
public:
    PredecodeCache();
    ~PredecodeCache();
    PredecodeCache(const PredecodeCache&) = delete;
    void RegisterCodeRange(const void* start, size_t size_bytes);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}

// include/gaby_vm/simulator.h
namespace gaby_vm {
class Simulator {
public:
    Simulator(PredecodeCache* cache, void* stack_buf, size_t stack_size);
    ~Simulator();
    void RunFrom(uintptr_t entry_pc);
    void WriteRegister(int idx, uint64_t value);
    uint64_t ReadRegister(int idx) const;
    // 按需补
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
```

理由：embedder 看不到 `vixl::*`，符合现有 spec 习惯。Pimpl 的间接调用代价只在
构造/析构和非 hot accessor，对 hot loop 性能没影响。

### 9. 文件分布

```
include/gaby_vm/
├── gaby_vm.h            (已有 facade)
├── version.h            (已有)
├── predecode_cache.h    (新)
└── simulator.h          (新)

src/gaby_vm/             (新目录)
├── predecode_cache.cc   (cache impl + 预解码 pass)
├── simulator.cc         (Pimpl 胶水)
├── thunks.h             (per-form thunk template 机制)
└── thunk_table.cc       (form→thunk 实例化的 list)

src/aarch64/             (imported VIXL，加注释式修改)
├── simulator-aarch64.h  (ExecuteInstruction 改造 + 加 cache 字段)
└── simulator-aarch64.cc (按需，可能改 GetFormToVisitorFnMap 让 cache 拿到表)
```

### 10. ExecuteInstruction 改造（注释式，非严格 marker）

```cpp
// simulator-aarch64.h:1401 附近
void ExecuteInstruction() {
    VIXL_ASSERT(IsWordAligned(pc_));
    pc_modified_ = false;
    // BType / guarded page check 不变...

    // gaby-vm: predecode cache hot path; fallback covers trace/visitor/debug/no-cache
    if (VIXL_LIKELY(predecode_cache_active_)) {
        DispatchViaPredecodeCache();
    } else {
        decoder_->Decode(pc_);
    }

    // ... 末尾 MOVPRFX check / IncrementPc / UpdateBType / auditor assert 不变
}
```

`predecode_cache_active_` 是一个 bool，下面任一情况翻成 false：

- cache 指针 nullptr
- trace 参数非 `LOG_NONE`
- `Decoder::AppendVisitor` 注册过非 baseline visitor
- debugger 启用

trace / visitor / debugger 的 setter 翻这个 bool。hot path 一次 bool 读完事。

## 还没敲死的小事（user 明确说要拍但来不及了）

### 11. Out-of-range PC

**Claude 推荐**：production `VIXL_ABORT_WITH_MSG("PC <addr> not in any registered
code range")`；debug build 可选 compile-time flag `GABY_VM_ALLOW_OUT_OF_RANGE` 回退
到 `decoder_->Decode(pc_)`。

待 user 拍：abort 是否太硬？bridge 场景下 VM 跳出去 native 是否可能意外留下
out-of-range 状态？

### 12. Decoder 生命周期

**Claude 推荐**：`PredecodeCache::Impl` 自己持有一个 `vixl::aarch64::Decoder`，
RegisterCodeRange 时用它跑 decode 树。Cache 跟 Decoder 同生共死。`Simulator::Impl` 不
独立持 Decoder，trace fallback 走 cache 暴露的 const Decoder 引用。多 Simulator 共享
同一份 Decoder。

待 user 拍。

### 13. Auditor 处理

**Claude 推荐**：cache 自带一个 `CPUFeaturesAuditor`，RegisterCodeRange 时对每条指令调
`auditor.Visit(...)`。遇到不可用指令 → RegisterCodeRange 返回 error code（C-ABI 友好；
具体形式待定）。注册成功后 hot path 不再 assert auditor 状态——末尾那行
`VIXL_CHECK(cpu_features_auditor_.InstructionIsAvailable())` 改成
`if (!predecode_cache_active_) VIXL_CHECK(...)`。

待 user 拍：embedder 能不能选择"准许部分 unsupported，记下来其他照常走"？返回 error
具体形式（错误码 / 异常 / errno-like）？

### 14. Shadow self-test（GABY_VM_DOUBLE_DECODE）

**Claude 推荐**：V1 实现一个 compile-time flag `GABY_VM_DOUBLE_DECODE`：开启时
ExecuteInstruction 两条路径都跑，对比 register + NZCV + PC + memory write 差异，不一致
就 abort。默认关，正常 build 零开销。这是 deep-dive R10 的唯一可靠应对。

待 user 拍：要不要强制进 V1（vs 后置到 V1.x）？对比哪些状态（仅寄存器？memory write 也对？
NZCV？FPCR？BType？SVE FFR？）？

## 完全没讨论到的开放问题

### 15. 并发模型细节（RegisterCodeRange 期间）

User 说补丁加载"比较低频"但运行期会发生。需要明确：

- 多线程同时跑（多 Simulator 实例）时新调一次 RegisterCodeRange，需不需要 stop the world？
- range 表的形态：fixed-capacity array + atomic count？append-only RCU？还是上锁？
- 注册期间其他 Simulator 实例 hot path 能不能继续跑（看 cur_range cache 是否还有效）？
- 同一段代码被重复 RegisterCodeRange，怎么处理（noop？warn？error？）

### 16. 跨 range 分支 + lookup 慢路径的精确语义

cold path 找新 range 时：

- 用什么数据结构？sorted array + binary search？radix tree？hash？
- range 数量上限要不要定？（fixed-capacity 4096？）
- 分支到完全没注册过的地址 = out-of-range，跟 §11 衔接

### 17. Bridge to native 的具体衔接

User 提到 vm-native-vm-native 交错。bridge 在 leaf 内通过 VIXL `RegisterBranchInterception`
拦截就退出 VM。需要确认：

- bridge 返回时 PC 怎么写回？必须落在某个已注册 range 内
- bridge 调用期间其他线程改 cache 有没有竞态
- LR 写入 → ret 回 VM 的具体路径

### 18. Cache 大小预算 + 内存策略

- 100MB 补丁代码 = 25M 指令 × 8 bytes = 200MB cache。要不要限制？
- 内存不够 fail 优雅退场，还是 abort？
- 有没有 `FlushCodeRange` API（卸载补丁的对偶）？

### 19. 测试集成

- 现有 `test/simulator_correctness.cc` / `test/simulator_smoke.cc` 跟新 cache 怎么整合？
- 现有 `bench/` 里 baseline benchmark 跟 cache enabled benchmark 的对比形式
- shadow self-test 跑在哪个 build target 里（专门加一个 `gaby_vm_shadow_test`？）

### 20. 错误处理 + 诊断

- RegisterCodeRange 失败的诊断信息（哪条指令 unsupported / 已注册重叠 / 大小不对）
- out-of-range abort 信息怎么打
- shadow self-test 失败的 diff dump

### 21. CPU features 配置

- PredecodeCache 构造时是否要传 CPUFeatures（决定 auditor 接受什么）？
- 多个 Simulator 共享 cache，是不是必须 CPU feature 一致？

### 22. 性能目标

- 期望的 dispatch overhead 降幅是多少？deep-dive 说 300-500 → 10 cycle 量级，但是否定义清晰
  的验收标准？
- 哪些 microbench / 现实 workload 作为验收？bench/ 里现有的 baseline 够不够？

## 已有的相关文档（一定要先读）

按重要性排：

1. **`docs/refs/gaby-vm-modification-sketch.md`** —— **总方向**。把 cache、多实例 + 真原子、
   embedder 栈、no-SMC 等一系列改造的方向决定串起来了。这一轮 brainstorm 是把它的 cache
   段落落地成可实施的设计。
2. **`docs/refs/vixl-fetch-decode-dispatch-deep-dive.md`** —— **性能分析 + 风险清单**。
   §3 cycle 拆解、§5 风险 R1-R12 是这次设计绕不开的。
3. **`docs/refs/vixl-decode-dispatch-pattern.md`** —— **事实参考**。每条指令的 VIXL 控制流，
   带 `path:line` 引用。设计时要找上游某个东西在哪行就查它。
4. **`docs/refs/vixl-aarch64-simulator-architecture.md`** —— **Simulator 总览**。寄存器、
   内存、auditor、disasm、debugger、trace 等子系统的整体表面。
5. **`AGENTS.md` / `CLAUDE.md`** —— 项目根目录，含 agent rules。注意 VIXL boundary
   政策已放宽（见上面 §5）。
6. **`docs/architecture.md`** —— 项目级架构。memory model / threading model / VIXL import
   boundary 三段尤其相关。
7. **`openspec/specs/aarch64-simulator/spec.md`** —— 现有 spec。byte-identical 那条以后
   要同步放宽。

## 下次 session 的几点提醒（给下一个 Claude 看）

1. **Plan mode 下 user 看不到 plan 文件**——要把决定都说在聊天里，不能假设 user 看见
   "我已经写进 plan 了"。
2. **这不是 OpenSpec change**，是普通 brainstorm，产出 `docs/refs/` 设计文档即可。
3. **中文打字仔细一些**——上个 session 多次出现"反原"/"一勺拍"/"话不多说"/"你拍果"这种
   错字，沟通效率被拖累。
4. **解释技术概念前先 check 共同理解**——上次 thunk 那段花了两轮 user 才明白。下次先用
   一段大白话引入"thunk = 一个小转发函数"，再上 template 代码。
5. **不要 over-recommend**——给推荐是好的，但要给 user 真实的选项空间，避免误以为只是
   "确认我的方案"。
6. **进度节奏**：上面 §1-5 是 user 明确确认的，§6-10 是上个 session 一路推过来 user 没否决
   但也没显式点头的。下个 session 可以快速复盘 §6-10 让 user 显式点头，再处理 §11-14，最
   后讨论 §15-22 的开放问题，然后开始写 `docs/refs/` 设计文档。

## 草稿设计文档的目标位置

`docs/refs/gaby-vm-predecode-cache-design.md`（暂定名）。结构建议参照
`docs/refs/gaby-vm-modification-sketch.md`：goal / constraints / what stays unchanged /
what changes / new structures / where it plugs in / risks / non-goals。

写完后 commit 到主项目，作为后面 OpenSpec change 的事实基础。
