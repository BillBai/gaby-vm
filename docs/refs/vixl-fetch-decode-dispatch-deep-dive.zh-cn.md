# VIXL AArch64 Simulator Fetch / Decode / Dispatch 深度分析与预解码缓存设计

> 这是 Gaby-VM 预解码缓存方向的工作文档。读者预期是要在 gaby-vm 里实现或 review
> 「predecode 一次 → 缓存 dispatch target → 重复执行」这条优化的人。
>
> 这份 doc **不是**事实参考（`vixl-aarch64-simulator-architecture.md` 和
> `vixl-decode-dispatch-pattern.md` 已经把代码路径和源码引用写得很扎实了），它
> 解决的是另外一类问题：当前路径慢在哪里、缓存条目应该长什么样、绕过 Decoder
> 时哪些状态不能丢、有哪些容易踩的坑。所有源码引用都用 `path:line` 的写法，方便
> 你打开 `../vixl/src/aarch64/<file>` 直接查证。

## 1. 一张图先说清楚整条链

VIXL 解释器的「一条指令一轮循环」是这样的形状：

```
Simulator::RunFrom(first_pc)               cc:840
   │
   ▼
Simulator::Run()                            cc:816
  while (!IsSimulationFinished()):          h:1362    pc_ == kEndOfSimAddress (NULL)
     │
     ▼
  Simulator::ExecuteInstruction()           h:1401
     pc_modified_ = false                   h:1404
     BType / guarded page check             h:1408–1418
     last_instr_was_movprfx = ...           h:1420–1421
        │
        ▼
     Decoder::Decode(pc_)                   cc:39       const-visitor assert + 一次根节点 dispatch
        │
        ▼
     CompiledDecodeNode::Decode(instr)      cc:1354     树游走：bit_extract → 表查 → 递归
        │ (每层：member-fn-ptr 调用 + 一次 array load)
        │ Root 采样 10 bits → 1024 entry 表（最宽的一层）
        │ ... 2 ~ 3 层中间节点 ...
        ▼
     <leaf>: Decoder::VisitNamedInstruction(instr, "<form>")  cc:137
        Metadata m = {{"form", name}}     ← 每条指令一次 std::unordered_map heap alloc
        form_hash = Hash(name.c_str())    ← 第一次 hash
        form_to_unalloc_ 多重映射查询
        for v in visitors_ (std::list):
            v->Visit(&m, instr)           ← 虚函数派发，典型 2~3 个 visitor
                │
                ▼
        Simulator::Visit(metadata, instr)  cc:2306
           form_hash_ = Hash(form.c_str()) ← 第二次 hash 同一个名字
           it = GetFormToVisitorFnMap()->find(form_hash_)   ← unordered_map 查
           (it->second)(this, instr)       ← 间接调用
              │
              ▼
        Simulator::VisitAddSubImmediate / Simulate_*(...)   leaf
           读位段 / 读寄存器 / 算 / 写寄存器 / 可能写 PC
     ─返回─
     last_instr_ = ReadPc()                h:1436
     IncrementPc()                         h:1437  → h:1379  if (!pc_modified_) pc_++
     LogAllWrittenRegisters()              h:1438
     UpdateBType()                         h:1439
     VIXL_CHECK(cpu_features_auditor_.InstructionIsAvailable())   h:1441
```

要点：

- 一条指令至少要走过 **1 次根 + 2 ~ 3 次中间节点 → 共 ~3 ~ 4 次 member function pointer
  调用**（树游走），加 **1 次 visitor list 遍历 + 2 ~ 3 次虚函数调用**（visitor 链），
  再加 **1 次 std::unordered_map find + 1 次 function pointer 调用**（form → leaf），
  最后才是真正的 leaf 语义。
- 还有 **1 次 std::unordered_map heap allocation**（`Metadata m`，
  `Decoder::VisitNamedInstruction` 入口处构造），以及 **同一个 form name 被 hash 两次**
  （decoder cc:141 一次、simulator cc:2315 又一次）。
- 离开 leaf 之后还有 PC 自增的 `pc_modified_` dance、BType 状态推进、auditor 强校验。

下面把这条链拆开讲，先用一条最普通的 `ADD x0, x1, #4` 把抽象步骤具象化，再做性能
账算和缓存设计。

## 2. 一条 `ADD x0, x1, #4` 的完整旅程

### 2.1 fetch：PC 就是宿主指针

VIXL 没有 guest 地址空间这一层抽象。`pc_` 的类型直接就是 `const Instruction*`
（`simulator-aarch64.h:5348`），是宿主进程里指向那条 32-bit 指令字的真指针。
`Instruction::GetNextInstruction()`（`instructions-aarch64.h:734`）就是
`return this + kInstructionSize;`——纯 C++ 指针算术。

这件事对预解码缓存非常关键：**缓存可以直接把 `const Instruction*` 当 key**，不需要
guest→host 地址翻译，也不需要 PC→cache-line 的 hash。后面 §5 会反复用到这个前提。

`Memory::Read`（`simulator-aarch64.h:393–411`）走的是 `memcpy(&value, base, sizeof(value))`，
对 instruction stream 同理——guest code 就是宿主进程的普通可读内存页，VIXL 直接
解引用。这意味着「self-modifying code」在 VIXL 这里相当于宿主代码改自己的数据页，
通常不会发生，但缓存层要明确假设这一点（见 §6 R6）。

### 2.2 进入 `ExecuteInstruction`：状态归零和 BType 检查

`ExecuteInstruction` 整个函数是 inline 在头文件里的（`simulator-aarch64.h:1401–1442`），
开头做三件事：

```cpp
VIXL_ASSERT(IsWordAligned(pc_));
pc_modified_ = false;                                            // h:1404

if (PcIsInGuardedPage() && (ReadBType() != DefaultBType)) {     // h:1408–1418
    // BTI / PACI[AB]SP 校验，否则 abort
}
```

`pc_modified_` 这个 flag（`h:5347`）必须在每条指令开头清零，因为后面 `WritePc()`
（`h:1369–1374`）做分支时会把它置 true，结尾的 `IncrementPc()` 看到 true 就跳过自增。
这是 VIXL 处理「指令是否改了 PC」的核心机制——非常简单，但**任何缓存路径都必须复刻
这个语义**，否则分支后 PC 会被多加 4。

BType / guarded page 检查是个 runtime state machine：BTI 区域里上一条指令决定本条
能不能执行。这一段不能 cache（依赖 runtime btype_），缓存路径要么继续每轮跑、要么
区域级别预先证明「不会进 guarded code」。

### 2.3 `decoder_->Decode(pc_)`：树游走

`Decoder::Decode`（`decoder-aarch64.cc:39–46`）只做两件事：debug-only 的 const-visitor
assert 循环 + 一次 `compiled_decoder_root_->Decode(instr)`。真正的活儿在
`CompiledDecodeNode::Decode`（`decoder-aarch64.cc:1354–1367`）：

```cpp
void CompiledDecodeNode::Decode(const Instruction* instr) const {
  if (IsLeafNode()) {
    decoder_->VisitNamedInstruction(instr, instruction_name_);
  } else {
    VIXL_ASSERT((instr->*bit_extract_fn_)() < decode_table_size_);
    VIXL_ASSERT(decode_table_[(instr->*bit_extract_fn_)()] != NULL);
    decode_table_[(instr->*bit_extract_fn_)()]->Decode(instr);
  }
}
```

每一层非叶子节点：**一次 member-function-pointer 调用**（提取本节点感兴趣的位段
并 compress 成一个紧凑 index）+ **一次表查**（`decode_table_[idx]`）+ **一次递归调用**
（虚函数实际是直接成员函数，但 `CompiledDecodeNode::Decode` 本身不是 virtual——
间接调用走的是 `bit_extract_fn_` 这个成员函数指针）。

ADD 这条指令在 Root 节点（`decoder-constants-aarch64.h:9682`）会怎么走？Root 一次性
采样 10 个位 `{31, 29, 28, 27, 26, 25, 24, 21, 15, 14}`，所以 Root 的 `decode_table_`
是 1024 项的扁平数组。`ADD x0, x1, #4` 的 32-bit 编码在这 10 位上是
`0b1010001000`（sf=1, S=0, op0123=1000, addsub-imm subclass bits, imm12 高位）。
Root 查到对应 entry 是某个内部节点 `_xxxxxx`，那个内部节点会再采样 1 ~ 2 个位
（区分 add/adds/sub/subs），最后落到 leaf node `add_64_addsub_imm`。**一条 ADD
imm 大约走 2 ~ 3 层就到叶子**。

> 关于 Root 节点宽度的小提示：10 个采样位 → 1024 entry × 8 字节指针 ≈ 8KB 一个表。
> 这已经超过一段 cache line，但完全装得下 L1 D-cache（典型 32 ~ 64KB）。中间节点都是
> 2 ~ 4 个采样位，单表只有几十字节，cache friendly。整个 decode 树的内存足迹是百 KB
> 量级（看 `decoder-constants-aarch64.h` ~10000 行就能感受到），不会喧宾夺主。

### 2.4 `Decoder::VisitNamedInstruction`：visitor fan-out

到达 leaf 后调 `Decoder::VisitNamedInstruction(instr, "add_64_addsub_imm")`
（`decoder-aarch64.cc:137–158`）：

```cpp
void Decoder::VisitNamedInstruction(const Instruction* instr,
                                    const std::string& name) {
  std::list<DecoderVisitor*>::iterator it;
  Metadata m = {{"form", name}};
  uint32_t form_hash = Hash(name.c_str());

  auto range = form_to_unalloc_.equal_range(form_hash);
  for (auto itu = range.first; itu != range.second; ++itu) {
    uint32_t mask = itu->second >> 32;
    uint32_t value = itu->second & 0xffffffff;
    if (instr->Mask(mask) == value) {
      m.insert({"unallocated", ""});
      break;
    }
  }

  for (it = visitors_.begin(); it != visitors_.end(); it++) {
    (*it)->Visit(&m, instr);
  }
}
```

这里有几件值得记住的事：

- `Metadata m = {{"form", name}}` 是 `std::unordered_map<std::string, std::string>`
  的栈上对象，但 unordered_map 的桶数组是 heap 上的——**每条指令一次 heap 分配**，
  外加 `name` 的 std::string 也可能短串优化 / 长串分配。
- `Hash(name.c_str())` 把 form name 哈希一遍。
- `form_to_unalloc_.equal_range` 在每次都跑（哪怕该 form 没有 unallocated 编码），
  multimap 查询不便宜。
- visitor 链表是 `std::list<DecoderVisitor*>`，每次循环一个虚函数调用。**默认配置
  下有 2 个 visitor**（CPUFeaturesAuditor + Simulator），开了 trace 之后是 3 个
  （多一个 PrintDisassembler）。

### 2.5 `Simulator::Visit`：第二次 hash 同一个 name，再查表，再跳转

`Simulator::Visit`（`simulator-aarch64.cc:2306–2323`）：

```cpp
void Simulator::Visit(Metadata* metadata, const Instruction* instr) {
  VIXL_ASSERT(metadata->count("form") > 0);
  if (metadata->count("unallocated") > 0) {
    VisitUnallocated(instr);
    return;
  }

  std::string form = (*metadata)["form"];
  form_hash_ = Hash(form.c_str());
  const FormToVisitorFnMap* fv = Simulator::GetFormToVisitorFnMap();
  FormToVisitorFnMap::const_iterator it = fv->find(form_hash_);
  if (it == fv->end()) {
    VisitUnimplemented(instr);
  } else {
    (it->second)(this, instr);
  }
}
```

这里发生：

- `(*metadata)["form"]` 一次 unordered_map 查找 + 一次 std::string 拷贝。
- `Hash(form.c_str())` **再 hash 同一个字符串一次**——这跟 cc:141 的那次完全重复，
  纯粹因为 Metadata 的 KV 是字符串，hash 没缓存。
- `fv->find(form_hash_)`：另一次 unordered_map 查找，桶数组是只读静态表（构造一次，
  见 `simulator-aarch64.cc:105`）。
- `(it->second)(this, instr)` 是真正的 leaf 调用——通过 `void(*)(Simulator*, const Instruction*)`
  函数指针，间接跳到 `Simulator::VisitAddSubImmediate` 的 thunk。

注意 `form_hash_` 是 Simulator 的成员变量（`h:5460` 附近），它在 leaf 内部还会被读
（`Simulate_*` 系列里大量 `switch (form_hash_)`，比如 `Simulator::Simulate_PdT_PgZ_ZnT_ZmT`
在 `cc:2325` 之后的 switch 块）。**这是缓存设计里最容易踩的坑**：cache 路径直接
跳 leaf 就必须先把 `form_hash_` 写正确，否则共享 `Simulate_*` 入口的多个 form 会走错分支。

### 2.6 `VisitAddSubImmediate` → `AddSubHelper`：又把位段抽一遍

`VisitAddSubImmediate`（`simulator-aarch64.cc:4182–4186`）：

```cpp
void Simulator::VisitAddSubImmediate(const Instruction* instr) {
  int64_t op2 = instr->GetImmAddSub()
                << ((instr->GetImmAddSubShift() == 1) ? 12 : 0);
  AddSubHelper(instr, op2);
}
```

立刻调 `AddSubHelper`（`cc:4129–4166`）：

```cpp
void Simulator::AddSubHelper(const Instruction* instr, int64_t op2) {
  unsigned reg_size = instr->GetSixtyFourBits() ? kXRegSize : kWRegSize;
  bool set_flags = instr->GetFlagsUpdate();
  int64_t new_val = 0;
  Instr operation = instr->Mask(AddSubOpMask);

  switch (operation) {
    case ADD: case ADDS:
      new_val = AddWithCarry(reg_size, set_flags,
                             ReadRegister(reg_size, instr->GetRn(), instr->GetRnMode()),
                             op2);
      break;
    case SUB: case SUBS:
      new_val = AddWithCarry(reg_size, set_flags,
                             ReadRegister(reg_size, instr->GetRn(), instr->GetRnMode()),
                             ~op2, 1);
      break;
    default: VIXL_UNREACHABLE();
  }

  WriteRegister(reg_size, instr->GetRd(), new_val,
                LogRegWrites, instr->GetRdMode());
}
```

VisitAddSubImmediate + AddSubHelper 加起来对同一条 32-bit 指令字调了 **8 次** 位段
抽取方法：`GetImmAddSub()`、`GetImmAddSubShift()`、`GetSixtyFourBits()`、
`GetFlagsUpdate()`、`Mask(AddSubOpMask)`、`GetRn()`、`GetRnMode()`、`GetRd()`、`GetRdMode()`。
这些方法本身不贵——`Get##Name(...)` 都是 `DEFINE_GETTER` 宏（`instructions-aarch64.h:308–312`）
inline 展开成 `ExtractBits(msb, lsb)`（`h:246`）→ `ExtractUnsignedBitfield32` 的位运算
——但**累加起来一条指令至少 8 次 shift+mask**，外加 `Instruction*` 的解引用必须每次
跨 cache line（虽然 32-bit 指令字本身一定 hot）。这是后面 §5.6 讨论 operand
pre-extraction 的动机。

`AddWithCarry` 真正算数加法 + 可能写 NZCV；`WriteRegister` 把结果写到 `registers_[code]`
里（默认配置下还要走一次 trace log 判断）。

### 2.7 退出 leaf：PC 自增 + auditor 校验

回到 `ExecuteInstruction`，剩下的尾巴是：

```cpp
if (last_instr_was_movprfx) {                                    // simulator-aarch64.h:1431
  VIXL_ASSERT(last_instr_ != NULL);
  VIXL_CHECK(pc_->CanTakeSVEMovprfx(form_hash_, last_instr_));
}

last_instr_ = ReadPc();                                          // simulator-aarch64.h:1436
IncrementPc();                                                   // simulator-aarch64.h:1437
LogAllWrittenRegisters();                                        // simulator-aarch64.h:1438
UpdateBType();                                                   // simulator-aarch64.h:1439

VIXL_CHECK(cpu_features_auditor_.InstructionIsAvailable());     // simulator-aarch64.h:1441
```

- `last_instr_` 记录刚执行完的指令地址，给下一轮的 MOVPRFX 校验用。SVE 的 MOVPRFX
  约定要求紧跟一条「能被合并的指令」，违反就 abort。
- `IncrementPc()` 看 `pc_modified_`：分支已经写过 PC 就不动；否则 `pc_ += 4`。
- `cpu_features_auditor_.InstructionIsAvailable()` 是个**强校验**：CPUFeaturesAuditor
  作为 visitor 在每条指令的 `Visit()` 里记录「这条指令需要哪些特性、当前 CPU 配置
  有没有」（`cpu-features-auditor-aarch64.h:107–118`），ExecuteInstruction 末尾断言
  必须可用，否则 abort。

### 2.8 一条 ADD 的开销账（粗算）

把上面这一轮加起来，不开 trace、单 visitor 配置（auditor + simulator）：

| 阶段 | 调用次数 / 开销 |
|---|---|
| BType 检查 | 几个 if，分支可预测 |
| 树游走（Root → leaf，约 3 层） | 3 次 member-fn-ptr 调用 + 3 次表查 + 3 次递归 |
| `VisitNamedInstruction` | **1 次 unordered_map heap 分配**（Metadata）、1 次 string hash、1 次 multimap range 查询、2 次 visitor 链表迭代 + 2 次虚函数调用 |
| `Simulator::Visit` | **再 1 次 string hash 同一个 name**、1 次 unordered_map find、1 次函数指针调用 |
| `VisitAddSubImmediate` + `AddSubHelper` | 8 次位段抽取、1 次 ReadRegister、1 次加法、1 次 WriteRegister（含 trace 判断） |
| 尾部清理 | `IncrementPc` / `UpdateBType` / `LogAllWrittenRegisters`（trace off 时近乎空） / `cpu_features_auditor_` 断言 |

从「真正改了状态」（一次寄存器读、一次加法、一次寄存器写）反推，**dispatch overhead
是 leaf 真实工作的 5 ~ 20 倍**。换句话说，对短小的指令（ADD、MOV、CMP、LDR 简单
寻址等），解释器超过 80% 的时间花在「找到 leaf」而不是「执行 leaf」。这正是预解码
缓存的目标。

## 3. 性能瓶颈的细账

§2 已经把开销点一个个点出来了，这一节用大致 cycle 量级排序，方便决定优先打哪一个。
**精度只到 order-of-magnitude**——现代 OoO CPU 的真实表现严重依赖分支预测和 cache
状态，更精细的数字得靠实测，硬掰反而误导。

### 3.1 单条指令的 cycle 拆解（粗估）

假设大多数 dispatch 间接调用都被 BTB 命中、L1d 命中，但仍有少量预测失败：

| 操作 | 单次 cycle 量级 | 一条 ADD 出现次数 | 小计 |
|---|---|---|---|
| L1 D-cache hit load | ~4 | ~10（位段抽取 + 表查 + 寄存器读） | ~40 |
| 间接调用（BTB 命中） | ~5 | ~6（树游走 + visitor + leaf） | ~30 |
| 间接调用（BTB miss / mispredict） | ~15–25 | 0 ~ 2（看代码 hot 程度） | 0 ~ 50 |
| std::unordered_map find | ~30–80（hash + bucket 步进） | 1（form_hash → fn）+ 1（Metadata 内部） | ~80 |
| std::string hash + 拷贝 | ~20–50 | 2（form name 被 hash 两次） | ~80 |
| Heap 分配（unordered_map 桶） | ~50–200 | 1（Metadata） | ~100 |
| 虚函数 + 链表迭代 | ~10 | 2 个 visitor | ~20 |
| Leaf 实际运算（ADD 的加法 + 寄存器写） | ~5–10 | 1 | ~10 |

总和 **300 ~ 500 cycle 量级，其中 leaf 真实工作只占 5%**。这跟 native 一条 ADD 在
现代 ARM 核上 1 cycle 完成相比，慢了 300 ~ 500 倍。这个比例符合 VIXL 一直以来作为
「教科书干净的解释器」而非性能解释器的定位。

### 3.2 hotspot 排序（按「执行频次 × 单次成本」）

按打掉之后能省多少 cycle 排序：

1. **`Decoder::VisitNamedInstruction` 的 Metadata heap 分配 + 双重 hash + visitor list
   迭代** —— 每条指令必发，单次 ~150–300 cycle。**最大 hotspot**。预解码缓存的
   核心目的就是把这一段整个绕过去。
2. **CompiledDecodeNode 树游走 ~3 层** —— 每条指令必发，单次 ~30–50 cycle。也是必须
   绕过的部分。
3. **`Simulator::Visit` 的 form_hash 查表 + 间接调用** —— 每条指令必发，~50–100 cycle。
   缓存条目里直接存 leaf 函数指针就消除了。
4. **leaf 内部位段重抽** —— 每条指令重复 5 ~ 15 次小位运算，~30–80 cycle。这部分**不在 V1
   缓存范围**，归 V2 的 operand pre-extraction（见 §5.6）。
5. **CPUFeaturesAuditor 的 Visit + 末尾 assert** —— 中等开销，但不能简单删——它防止
   guest 用了不该用的特性。可以缓存结果（见 §6 R3）。
6. **PC alignment assert / pc_modified_ flag dance** —— 已经够轻，可以原样保留。

### 3.3 可摊销 vs 不可摊销

| 操作 | 是否可摊销 | 备注 |
|---|---|---|
| 树游走结果（leaf fn） | ✅ 完全可摊销 | 同一条指令字永远走到同一个 leaf |
| form_hash | ✅ 完全可摊销 | 字符串 → uint32 是纯函数 |
| Metadata heap alloc | ✅ 完全可摊销 | 直接砍掉，缓存路径不需要 Metadata |
| visitor 链表迭代 | ⚠️ 部分可摊销 | 默认配置可缓存，trace / 自定义 visitor 时必须 fallback |
| 位段抽取（leaf 内部） | ✅ V2 可摊销 | 但每个 leaf 抽不同的位段，需要 per-form 的 slot 布局 |
| BType / guarded page 检查 | ❌ runtime state | 每轮必须执行 |
| `pc_modified_` / `IncrementPc` | ❌ runtime state | 每轮必须执行 |
| `cpu_features_auditor_` assert | ⚠️ 可预先验证 | region 级别预证后可关 |
| `last_instr_` for MOVPRFX | ❌ 跨指令依赖 | 每轮必须更新 |
| Leaf 实际语义（寄存器读写） | ❌ 不可摊销 | 这是真正要执行的工作 |

### 3.4 二级效应

- **Branch predictor 压力**：每条指令至少 5 ~ 7 次间接调用，分布在 decoder / visitor /
  leaf 三段代码里。如果 guest 代码包含很多种类的指令（典型现代 binary），BTB 容量
  会成为瓶颈——一条指令被换出预测表，下次它来时就要付预测失败成本。预解码缓存把
  6 次间接调用压成 1 次（缓存命中后只剩 leaf 调用），BTB 的负担直接降一个量级。
- **I-cache 压力**：decoder + decoder-constants + 所有可能调到的 visitor + leaf
  helper，热路径代码几十 KB 量级，刚好能把 L1i 撑满。简化了 dispatch 之后只剩 leaf
  代码热，I-cache 命中率会显著提升。
- **D-cache 压力**：`decode_table_`（树的各层）、`form_to_visitor_fn_map_` 静态表、
  visitor 链表节点——这些是 dispatch 路径上的 D-cache 占用。砍掉这条路径后，D-cache
  全留给 guest data 和 simulator state。
- **Allocator 抖动**：每条指令一次 unordered_map heap 分配是真实代价——allocator 内部
  的 lock / freelist 管理 / cache miss 都算上。长跑测试里这是稳定的 RSS 增长源（虽然
  立即被 free，但 fragmentation 是潜在问题）。

## 4. 预解码缓存设计 insights

下面是给 V1 实现的具体建议。**目标**：dispatch 开销从 300 ~ 500 cycle 量级降到
~30 cycle 量级（一次表查 + 一次间接调用），**前提是不改任何 leaf 语义**。

### 4.1 关键前提（只此一条，但是基石）

**`pc_` 是宿主指针，`GetNextInstruction()` 是宿主指针算术**——所以缓存可以直接以
`const Instruction*` 为 key，不需要任何翻译/hash。这件事的来源是
`simulator-aarch64.h:5348` 的字段类型 + `instructions-aarch64.h:734` 的
`return this + kInstructionSize;`。

如果 V2 之后要支持「guest 代码搬家 / dlopen / 多个独立的 region」，缓存只需要在
region 卸载时清掉对应 page 的 slot，不需要重建 hash。

### 4.2 缓存条目结构（V1 提案）

```cpp
struct DecodedSlot {
  // V1 字段
  using LeafFn = void (*)(Simulator*, const Instruction*);
  LeafFn   leaf_fn;     // 命中时直接调，绕过 visitor 链
  uint32_t form_hash;   // 必须在 call leaf_fn 前写回 simulator->form_hash_

  // 状态位（≤ 1 字节，可挤进 form_hash 的 padding）
  uint8_t  state;       // 0=empty, 1=valid, 2=unallocated, 3=unimplemented, 4=fallback

  // V2 可选：per-form 的 pre-extracted operand cache
  // uint32_t op_cache;   // 含 Rd / Rn / Imm 等，按 form 解释
};
// V1 sizeof ≈ 16 bytes（cache-line friendly）
```

`state` 字段对应几种走 slow path 的情况：
- `unallocated`：解码后是 unallocated 编码，每次都走 `VisitUnallocated`，slot 不缓存
  fn 指针、状态位强制 fallback。
- `unimplemented`：跟 unallocated 类似，但是是 `VisitUnimplemented`。
- `fallback`：例如启用了 trace 或注册了自定义 visitor，强制走原 `decoder_->Decode(pc_)`。

### 4.3 存储形态：per-page 桶（推荐）

三个选项及取舍：

- **选项 1：全局哈希表，key = `Instruction*`**。最简单，但每次访问要 hash + 探测，
  hash 函数本身的 cycle 可能跟省下来的差不多，得不偿失。
- **选项 2：per-page bucket（推荐）**。每个宿主 page（4KB / 16KB）维护一个
  `DecodedSlot[1024 或 4096]` 数组，索引 = `(pc - page_base) / 4`。lookup 是
  `page_table[pc_page]->slots[(pc & page_mask) / 4]`，**两次 load + 一次乘移**。
  跟 PC 算术天然 1:1 对齐，cache friendly，回收时直接 free 整个 page 的 slot 数组。
- **选项 3：per-region 预扫**。如果 guest code 的 region 边界已知（比如启动时一次性
  扫完整个可执行段），可以把整个 region 平铺成一个数组，零 hash 零探测。最快，但
  要求知道 region 边界，且对动态加载的代码（dlopen）支持差。

V1 推荐选项 2。page_table 用 `std::unordered_map<uintptr_t, std::unique_ptr<PageSlots>>`
或者更紧凑的 radix tree。命中路径在 hot loop 里就两次 load。

### 4.4 热路径伪代码

替换 `ExecuteInstruction`：

```cpp
void Simulator::ExecuteInstructionCached() {
  VIXL_ASSERT(IsWordAligned(pc_));
  pc_modified_ = false;

  // BType / guarded page 仍要执行（runtime state machine，不能 cache）
  MaybeBTypeCheck();

  bool last_instr_was_movprfx =
      (form_hash_ == "movprfx_z_z"_h) || (form_hash_ == "movprfx_z_p_z"_h);

  DecodedSlot* slot = predecode_cache_.LookupOrInsertEmpty(pc_);
  if (VIXL_LIKELY(slot->state == DecodedSlot::kValid)) {
    form_hash_ = slot->form_hash;          // ← R1: load-bearing for Simulate_*
    slot->leaf_fn(this, pc_);
  } else {
    SlowPathDecodeAndPopulate(pc_, slot);  // 走完整 decoder，把结果回填进 slot
  }

  if (last_instr_was_movprfx) {
    VIXL_ASSERT(last_instr_ != NULL);
    VIXL_CHECK(pc_->CanTakeSVEMovprfx(form_hash_, last_instr_));
  }

  last_instr_ = ReadPc();
  IncrementPc();
  // LogAllWrittenRegisters / UpdateBType / auditor check
  // 视模式保留——具体 cache 模式见 §5.5 R3
}
```

### 4.5 慢路径（cache miss）做什么

第一次见到一个 PC，或者 slot 被回收过，就走慢路径：

```cpp
void Simulator::SlowPathDecodeAndPopulate(const Instruction* pc, DecodedSlot* slot) {
  // 直接复用现有 decoder，不动它
  decoder_->Decode(pc);
  // 这一步会触发 Simulator::Visit -> set form_hash_ -> dispatch leaf

  // 复用 Visit 内已经写好的 form_hash_ 和 GetFormToVisitorFnMap()->find 结果，
  // 把 (form_hash_, leaf_fn) 存进 slot
  slot->form_hash = form_hash_;
  auto* fv = Simulator::GetFormToVisitorFnMap();
  auto it = fv->find(form_hash_);
  if (it != fv->end()) {
    slot->leaf_fn = it->second;
    slot->state = DecodedSlot::kValid;
  } else {
    slot->leaf_fn = nullptr;  // unimplemented
    slot->state = DecodedSlot::kUnimplemented;
  }
  // 处理 unallocated / fallback 状态参考 R12
}
```

**这一段的妙处是不动 decoder**——慢路径走的是原 VIXL 完整路径，正确性自动等价
原解释器；只是顺手把结果记下来，下次直接用。

### 4.6 V2：operand pre-extraction（不在 V1 范围）

V1 命中时还要走一次 `VisitAddSubImmediate → AddSubHelper`，里面会重抽 8 次位段。
V2 可以在 slot 里加一个 per-form 联合体，把 Rd / Rn / Imm 等预先抽好：

```cpp
struct ExtractedAddSubImm {
  uint8_t rd : 5;
  uint8_t rn : 5;
  uint16_t imm12;
  uint8_t shift_12;
  uint8_t sf;
  uint8_t set_flags;
  uint8_t op;
};
```

然后 leaf 的「快速版本」直接读 slot 字段，不再调 `instr->GetXxx()`。

V2 的成本：**每个 leaf 都要写一个对应的 fast-path 版本**，工程量大；好处是单条指令
能再省 30–80 cycle。V1 不做，但 slot 结构留好扩展空间。

## 5. 风险与挑战清单

排序大致按「踩中后修复成本」从高到低。每条给「问题描述 / 触发条件 / 缓解策略 /
源码引用」四件套。

### R1：`form_hash_` 必须在调 leaf 前还原

**问题**：很多 leaf 是 `Simulate_*` 共享入口，内部 `switch (form_hash_)` 选具体
行为。如果缓存路径直接调 leaf 而忘了写 `form_hash_`，会跑到 `default: VIXL_UNIMPLEMENTED()`
或者更糟——选错分支。

**触发**：任何使用共享 `Simulate_*` 入口的指令族，例如 SVE 的 `Simulate_PdT_PgZ_ZnT_ZmT`
（`simulator-aarch64.cc:2325`）。

**缓解**：`form_hash` 进 slot，缓存路径调 leaf 前 `form_hash_ = slot->form_hash;`。
**已经在 §4.4 的伪代码里体现，但实现时一定要写测试覆盖共享 leaf 的多个 form**。

**引用**：`simulator-aarch64.cc:2306-2323`（Visit 里写 `form_hash_`）+ `cc:2325` 之后
的 switch。

### R2：MOVPRFX 链式校验

**问题**：SVE MOVPRFX 要求紧跟一条「能合并」的指令。`ExecuteInstruction` 末尾
（`simulator-aarch64.h:1431-1434`）会拿上一条指令的 form_hash 验证。**这要求
`last_instr_` 和 `form_hash_` 跨缓存命中也能正确传递**。

**触发**：SVE 程序里出现 MOVPRFX 序列。

**缓解**：`form_hash_` 一定要在 leaf 调用前/调用中正确 set；`last_instr_ = ReadPc()`
保持在循环尾部（伪代码已保留）。**不要省掉 `last_instr_was_movprfx` 的提前计算**。

**引用**：`simulator-aarch64.h:1420-1421` + `h:1431-1434`。

### R3：CPU Features Auditor 必须满足

**问题**：`ExecuteInstruction` 末尾 `VIXL_CHECK(cpu_features_auditor_.InstructionIsAvailable())`
（`simulator-aarch64.h:1441`）会 abort。如果缓存路径绕过 visitor 链，auditor 没跑过，
这个断言要么永远成立（auditor 状态没变）要么挂掉。

**触发**：guest 代码用了配置 CPU 不支持的指令；或缓存路径错误地保留了上一条指令的
auditor 状态。

**缓解（任选）**：
1. **Region 级预验证**：在 cache populate 时同步跑一次 auditor，把「该 form 是否
   可用」存进 slot。可用就 valid，不可用就把 slot state 标 `fallback` 强制走慢路径，
   慢路径自然会 abort。
2. **保留 auditor 调用**：缓存路径仍然调 `cpu_features_auditor_.Visit(&meta, instr)`
   但用预构造的 `Metadata`（不每次重 alloc）。
3. **关掉 auditor**：embedder 显式声明「相信这段代码」，删掉 ExecuteInstruction
   末尾的 assert。**不推荐，破坏 VIXL 的安全契约**。

**引用**：`simulator-aarch64.h:1441` + `cpu-features-auditor-aarch64.h:107-118`。

### R4：trace / debugger / 自定义 visitor 模式

**问题**：开了 `LOG_DISASM`（`simulator-constants-aarch64.h:123-135`）后必须先调
`PrintDisassembler::Visit`；启用 debugger 后每条指令前要查 breakpoint；用户通过
`Decoder::AppendVisitor`（`decoder-aarch64.cc:90`）注册了自定义 visitor 也都要被调。
缓存路径默认绕过了 visitor 链。

**触发**：trace 模式打开 / debugger 启用 / 第三方 visitor 注册。

**缓解**：在 `Simulator::SetTraceParameters`、`SetDebuggerEnabled`、
`Decoder::AppendVisitor` 里设置一个全局 fallback flag，命中路径开头判断该 flag——
为 true 就直接走 `decoder_->Decode(pc_)`。**简单粗暴，但稳**。

**引用**：`simulator-aarch64.h:1423-1428`（visitor 顺序约定）+ `decoder-aarch64.cc:90-135`。

### R5：BType / Guarded Page 状态机

**问题**：BType 是 runtime state（每条指令推进），`PcIsInGuardedPage()` 是配置查询。
不能 cache 到 slot 里。

**触发**：guest 代码用了 BTI 指令、跑在 guarded page 上。

**缓解**：缓存路径仍然在循环开头执行 BType 检查（伪代码 `MaybeBTypeCheck()`）。开销
可以忽略——一次 if + 偶尔的 mask 比较。

**引用**：`simulator-aarch64.h:1408-1418` + `h:1396` 的 `PcIsInGuardedPage`。

### R6：Self-Modifying Code

**问题**：VIXL 直接读宿主内存做 fetch。如果宿主 / guest 改写了 code page，缓存里
的 slot 还是旧 leaf。

**触发**：guest 代码段被改写——通常应用层 binary 不会，但 JIT-style 的 guest（V8、
LuaJIT 之类）会自己生成代码到 RW page 然后 mprotect 成 RX。VIXL 不支持执行 mprotect
后的 page（缺乏 `__builtin___clear_cache` 等同物），但缓存层应该明确这个边界。

**缓解（V1）**：文档明确写出「guest code immutable」假设。embedder 如果违反，
负责调 `Simulator::InvalidateCacheRegion(begin, end)`（要新增）。

**缓解（V2）**：在 `Memory::Write` 命中 code region 时自动失效对应 slot。需要
`code region` 的边界已知。

**引用**：`simulator-aarch64.h:393-411`（Memory::Read）说明 fetch 是 memcpy，没有
监控点。

### R7：多实例 / 共享内存

**问题**：Gaby-VM 计划支持多个 Simulator 实例并行（参见 `gaby-vm-modification-sketch.md`），
guest code 可能被多个实例共享读。如果 cache 是全局共享的，需要锁或者 lock-free
设计；如果是 per-instance 的，又要付重复 populate 成本。

**缓解**：**V1 推荐 per-instance cache**。每个 Simulator 实例自己的 cache，互不干扰。
代价是同一段 code 被多个实例独立 populate，但 populate 本身在 cold path，影响很小。
V2 可以考虑跨实例共享只读的 leaf-fn 部分（form_hash + leaf_fn 是确定性的，不依赖
实例状态）。

**引用**：`vixl-aarch64-simulator-architecture.md` 的 「Threading and determinism」段。

### R8：Cache 大小失控

**问题**：长跑的 guest 会让 cache 无界增长。1MB code 全 populate ≈ 256K slot ≈ 4MB
缓存。10MB code 就是 40MB——还在可接受范围，但 100MB code（典型大型游戏）就要 400MB，
不能忽略。

**缓解**：
- per-page bucket 设计自带 region 粒度回收，guest 调 `mprotect` / unmap 时清掉对应
  page 的桶。
- 设置 cache 上限 + LRU page-level 替换：超过上限时丢最久没用的 page bucket。
- 暴露 `Simulator::FlushPredecodeCache()` 供 embedder 显式清。

### R9：iOS / no-JIT 合规

**问题**：iOS 不允许 RWX 页 / runtime codegen，这是 Gaby-VM 选择不做 JIT 的根本原因。
预解码缓存看似「生成了什么」，要明确它**不是 codegen**。

**结论**：缓存项是 ordinary data（结构体数组）。`leaf_fn` 是 .text 段里已经存在的
函数指针（VIXL 编译时已经写好的 `Simulator::VisitAddSubImmediate` 等）。**不分配
executable memory，不调用 `mprotect(PROT_EXEC)`，不需要 `pthread_jit_write_protect_np`**。
iOS 上完全合规。

**写到 doc 里**是为了避免后续 review 时被误判为「JIT」——这是个很容易引发不必要
争论的术语问题。

### R10：正确性回归

**问题**：缓存路径必须 byte-identical 复现非缓存路径的所有副作用——寄存器、
PSTATE/NZCV、内存、PC、btype_、ffr_register_。任何遗漏都是难发现的 bug。

**缓解**：开发期开 **shadow mode**——同一个 Simulator 同时跑两条路径（一条 cache，
一条 reference decoder），每条指令后 diff 全部状态。差异立即 abort。性能差 100×，
但用来跑 testsuite 是合理代价。

**引用**：`simulator-aarch64.cc:5xxx` 区间的 `LogAllWrittenRegisters` 之类的 helper
可以复用做 state snapshot 的基础。

### R11：BR / RET / 间接跳转

**问题**：缓存只能加速「已经 fetch 过的 PC」。`BR x0` / `RET` / `BLR x0` 跳到
runtime-计算出来的目标，第一次去到一个 PC 时仍然要走慢路径。

**影响**：典型 hot loop 几乎全是直接跳转（CBZ / B），间接跳转主要在函数调用边界
（`BLR` for indirect call、`RET` for return）。前者命中率高，后者每次返回都要走慢
路径——但 RET 的目标通常也是 hot 的（caller 之前 BLR 过，缓存里已有），所以二次
访问就是 cache hit。

**结论**：V1 不解决。V2 可以考虑 indirect-target predictor（按调用点缓存最后跳过去
的 PC）或者 basic-block linking（把跳转目标的 leaf 预编进当前 block 的尾部）。

### R12：unallocated / unimplemented 编码

**问题**：当前 decoder 通过 `Decoder::VisitNamedInstruction` 里的 `form_to_unalloc_`
检查（`decoder-aarch64.cc:145-153`）和 `Simulator::Visit` 里的 unimplemented 分支
（`simulator-aarch64.cc:2318`）处理这两类。如果缓存把它们当普通 form 缓存了 leaf
函数指针，会跑错（unallocated 应该 abort，不是执行）。

**缓解**：slot.state 引入 `kUnallocated` / `kUnimplemented` 两种状态，命中后强制
走对应处理路径（`VisitUnallocated` / `VisitUnimplemented`），而不调 leaf_fn。

**引用**：`simulator-aarch64.cc:3915`（VisitUnallocated）+ `cc:3907`
（VisitUnimplemented）+ `cc:3896`（VisitReserved）。

## 6. 跟其他 doc 的关系 + 后续阅读

- 想看每个 layer 的精确 file:line 引用和函数体：
  [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md)。本 doc 跟它
  互补——它是「事实」，本 doc 是「分析与设计」。
- 想看 Simulator 整体表面（构造、寄存器、内存、debugger 等）：
  [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md)。
- 缓存设计的更上层规划（cache 与 Gaby-VM 其他改动如何配合，包括多实例并发、real
  atomics、embedder-allocated stack 等）：
  [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)。
- VIXL 文件的导入清单：
  [`vixl-extraction-map.md`](./vixl-extraction-map.md)。
- 想要 visualize 整条链：[`vixl-fetch-decode-dispatch-deep-dive.html`](./vixl-fetch-decode-dispatch-deep-dive.html)
  （本 doc 的可视化版本，浏览器打开即用）。
