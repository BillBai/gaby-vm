# vixl_port 现场汇编重写 — Code Review 记录 — 2026-06-10

> **它是什么**：对 `vixl-port-live-assemble-rewrite` 这次改动（当时尚未提交，工作树
> vs `8859018`）的一份完整 code review 存档。审查方式是三个独立视角的深度静态审查
> （通用质量与拷贝忠实度 / 测试充分性 / 设计合理性）+ 审查者亲自编译、双配置运行、
> 新开 coverage 构建并产出覆盖率。它记录的是**这套护栏在 2026-06-10 这个时点的真实
> 状态、已知缺陷（带实证）、和按优先级排好的行动清单**。
>
> **它不是什么**：不是排期表，不是 OpenSpec change，也不是对设计稿的否定——骨架是被
> 认可的。源码引用用 `path:line`，路径相对仓库根；`../vixl` 指参考 VIXL 树（钉死
> SHA `160c445`）。文中明确区分「审查者实测坐实」与「审查分析静态推断」两类证据。
>
> **状态**：审查完成。四个 Critical 是「把这套套件当作 perf 改动放行闸门之前必须先
> 修」的硬门槛，合计约一两天工作量。被审改动本身可以合并（清掉几个文档/提交卫生问题
> 即可）。配套设计稿见文末「索引」。

## TL;DR

- 这次重写把旧「冻结 fixture 回放」模型「访存语义整体是盲区」的结构性死角从根上治掉
  了（旧模型结构性丢弃 308/595 个 body、load/store 零覆盖）。**589 个上游 TEST 里 485
  个真跑且双轨双 oracle 全过、0 失败**，Debug/Release 双配置一致。
- 隔离工程无可挑剔：no-RWX / ODR / license / 不出货边界全部核查无破口，孤岛 21 个拷贝
  文件对上游 `160c445` 逐文件 diff **零未记录漂移**。
- 扣分全部集中在一句话：**「绿灯」会在某些情况下撒谎**。有四条「出了问题却悄悄记成
  skip、测试照样绿」的通道（下文 C1–C4），其中两条（C2 / C3）审查者已用实测当场坐实。
  这恰好侵蚀护栏最值钱的性质——而你接下来要做的激进 dispatch 优化，最需要的就是它
  「出事就响」。
- 设计骨架（三引擎、现场汇编、双 oracle）是对的，设计审查打 **7/10，修完四个
  Critical 后约 8.5–9**。

## 0. 先建立心智模型：这套测试到底在做什么

理解这份记录的钥匙，是先想清楚三个「翻译官」：gaby 要执行一段 ARM 机器码，可以想象成
三个引擎在翻译同一段代码——

- **参考 sim**（VIXL 原版模拟器）：标准答案。
- **decoder 轨**：老实人，每条指令都查字典翻译（`DebugRunFrom`）。
- **cache 轨**：抄近路的，第一次翻译完把结果记小本本上（`PredecodeCache`），之后直接念
  小本本（`RunFrom`）。

你后续所有性能优化，都是让「抄近路的」越来越激进地抄近路。这套测试的活儿就是抓几百段
真实 ARM 代码让三个引擎各翻一遍、**对答案**——抄近路抄错了，对答案就露馅。

**贯穿全文的一个关键事实**（理解了它，后面 C3/C4 为什么严重就通了）：

> 参考 sim 和 gaby 两轨**共用同一份「每条指令具体怎么算」的代码**（都是从 VIXL 导入的
> 同一个 Simulator）。区别只在「怎么找到下一条该执行的指令」——参考和 decoder 走经典
> 路径，cache 走小本本。
>
> 所以测「抄近路抄错了没有」（dispatch / cache 回归）——**很强**；测「某条指令的算法
> 本身写错了」（leaf 语义）——**很弱**（三方共用一份代码，错也一起错）。整个系统里
> 唯一真正独立的标准答案，是上游测试里**手写死的那些期望数字**，比如
> `ASSERT_EQUAL_64(0x1234, x0)` 里的 `0x1234`。

还有一句也要先立住：**这套护栏的全部价值是「绿灯 = 真的没事」。** 第三节的四个 Critical，
全是「绿灯撒谎」的不同方式。

## 1. 实测结果

Debug 与 Release 双配置 `ctest` 17/17 全绿。`vixl_port` 三族（实测，两配置数字一致）：

| family  | 上游 TEST | 真跑且通过 | 跳过 | 失败 |
|---------|-----------|------------|------|------|
| integer | 258       | 185        | 73   | 0    |
| fp      | 76        | 64         | 12   | 0    |
| neon    | 255       | 236        | 19   | 0    |
| **合计** | **589**  | **485 (82%)** | **104** | **0** |

- 104 个 skip 全部归因（与代码逐类对账）：特性 deny 24、按名隔离 22、未调 RUN() 10、
  **FP16 断言 24**、信号兜底 7、**「ref 与字面量不符」8**、**cache 拒收 5**、指令上限 4。
  加粗三类是下文问题的主角。
- 运行成本：Debug 全套 7.5s，**Release 仅 0.39s**——便宜到可以每次改动随手跑。
- 稳定性：跨 ≥4 次独立运行（Debug、Debug+coverage 插桩、Release、trace 各一遍），
  通过/跳过集合完全一致；理论上的 ASLR skip 漂移在本机未观测到。帧窗口机制在 -O2 与
  coverage 插桩下经验上成立。
- **tasks.md 数字裁决**：`tasks.md` 5.3/6.2/6.3 记录的 185/73、64/12、236/19 与实测
  吻合；7.2 的「164/64/215」是迁移中期的陈旧快照，应以本次实测为准统一修正（见 I2）。

## 2. 覆盖率（本次新开启，未改任何源文件）

复现方式（独立构建目录 + 命令行注入编译器开关，源码零改动）：

```sh
cmake -S . -B build/coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DGABY_VM_BUILD_TESTS=ON -DGABY_VM_BUILD_BENCHMARKS=OFF -DGABY_VM_BUILD_DEMOS=OFF \
  -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
  -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
  -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate"
cmake --build build/coverage
LLVM_PROFILE_FILE="$PWD/build/coverage/profraw/vp-%p.profraw" \
  ctest --test-dir build/coverage -R vixl_port
xcrun llvm-profdata merge -sparse -o build/coverage/vixl_port.profdata \
  build/coverage/profraw/*.profraw
xcrun llvm-cov report build/coverage/test/test_support/vixl_asm/vixl_port_integer \
  -object build/coverage/test/test_support/vixl_asm/vixl_port_fp \
  -object build/coverage/test/test_support/vixl_asm/vixl_port_neon \
  -instr-profile=build/coverage/vixl_port.profdata "$PWD/Sources/gaby_vm"
```

核心数字（仅 vixl_port 三族驱动，针对出货库 `Sources/gaby_vm`）：

| 范围                                   | Region | 函数  | 行    |
|----------------------------------------|--------|-------|-------|
| `Sources/gaby_vm` 全体                 | 40.2%  | 53.8% | 52.4% |
| simulator-aarch64.cc（leaf 语义本体）  | 31.0%  | 33.5% | 40.0% |
| **simulator-aarch64.cc 剥离 SVE 后**   | 49.4%  | —     | **63.7%** |
| decoder-aarch64.cc                     | 76.1%  | 80.0% | **95.9%** |
| predecode_cache.cc（优化主目标）       | 80.3%  | 81.3% | 61.2% |
| logic-aarch64.cc（NEON/FP 元素运算）   | 50.4%  | 67.0% | 55.1% |

怎么读这张表：

- **vixl_port 就是这个库的覆盖主力**：全部 17 个测试一起跑，`Sources/gaby_vm` 行覆盖也
  只从 52.4% 涨到 52.8%。
- 表面 40% 的 simulator 覆盖被 SVE 死重严重拉低：231 个 `Visit*` leaf 中 153 个零覆盖，
  **其中 143 个是 SVE**（项目范围内明确不跑，纯死重——建议从指标分母剥离或文档注明）。
- **真正未覆盖的非 SVE leaf 只有 10 个**：`VisitReserved/Unimplemented/Unallocated/
  Exception`（trap/异常路径 4 个）、`VisitCryptoAES/2RegSHA/3RegSHA/SM3/SM4`（crypto
  5 个）、`VisitNEON2RegMiscFP16`（1 个，正对应被丢的 FP16 断言）。范围内 Visit 覆盖
  约 **78/88 ≈ 89%**。
- 差分测试场景里覆盖率的读法和普通单测不同：这里「行覆盖 63.7%」不是「还有 36% 没断
  言」，而是「这些行在三个引擎上各跑了一遍且终态被对拍」——同一行 leaf 被 cache 轨 /
  decoder 轨 / 参考 sim 各执行一次，一次覆盖 = 三重一致性证据。所以补覆盖应按「leaf
  类别是否整类缺席」排（crypto、FP16、trap），而不是按行数缺口排。

产物：HTML 报告 `build/coverage/html/index.html`，逐文件文本
`build/coverage/report_vixl_port_per_file.txt`。

## 3. 问题清单（按严重度，全部带实证）

测试遇到搞不定的题有两种处理：**FAIL（报红）** 和 **skip（作废不算分、继续）**。skip 本身
合理，问题是有四条路径把**本该报红的事悄悄记成了 skip**，于是绿灯撒谎。

### Critical — 当作 perf 放行闸门之前必须先修

**C1. 翻译官把试卷戳穿了，监考记「这题作废」而不是「出事了」**
（`gaby_two_track_main.h:183-195`；根因 `Sources/gaby_vm/src/gaby_vm/simulator.cc:81-115`）

cache 轨算错下一条指令地址会表现为跳飞 / 越界读内存 / 死循环，在 OS 层就是崩溃信号
（SIGSEGV）或超时（SIGALRM）。当前信号守护一律记成 `skip: aborted during run`，套件
照绿。**而你做激进 dispatch 优化最可能犯的错恰恰就是这一类**——老实人轨几乎不会犯，
只有抄近路的会。本该是最响的警报，现在是最沉默的跳过。旧方案反而没这毛病：崩溃就是进
程死 = CTest FAIL。这是新方案唯一一处护栏强度净退化。

连带伤害：崩溃用 `siglongjmp` 跳回主循环，跳过 C++ 析构，导致引擎内 `ExecutionScope` 的
`busy` 标志永久 latch——**从崩溃那一刻起，该族后面每道题都用半残引擎在答**，监考还在记
分。换句话说，套件刚抓到真 bug 的那一刻，后面整批结果就全部不可信，而报告不会告诉你。
还与 `design.md:110-112`「新踩到未实现 leaf 会 *fail*」的承诺直接矛盾（实际
`VisitUnimplemented` abort → skip）。

> 修法：TwoTrackRun 设阶段标志（assemble / ref / cache / decoder），信号落在 gaby 轨 →
> 记 FAIL 并终止该族（或重建引擎）。约 40 行。

**C2. 一个测试改了全局状态忘了改回来，连累后面一串题**（实测坐实）
（`gaby_two_track_macros.h:854-867` 的 START() 漏复位；根因：导入 simulator 的
`ResetState()` 不复位 `guard_pages_`）

背景：BTI（Branch Target Identification）是 ARM 安全特性——代码页标成「受保护」后不能
随便跳进中间，必须落在专门的落点指令。几个 BTI 测试会把**被反复复用的参考 sim** 切到受
保护模式，**测完没切回来**（START() 复位了 CPUFeatures / seen-features / GCS，恰好漏了
这一项）。于是参考 sim 从此永远停在受保护模式，后面含普通跳转（BR/BLR）的测试一跳就被
当成非法跳转报错 → 记 skip，gaby 两轨因为崩在参考阶段压根没跑。

**实测坐实**：运行时 stderr 冒出 3 条 `Executing non-BTI instruction with wrong BType`，
对应正是 `blr_lr`、`branch_tagged`、`branch_and_link_tagged` 三个被跳过的 case（第 4 个
受害者 `branch_tagged_and_adr_adrp` 先死于 adrp 断言）。受害者全是 **tagged pointer
分支**相关——而 tagged pointer 在 iOS 上到处都是（ObjC isa 指针、PAC 高位打标签）。这是
EL0 用户态护栏最不该丢的覆盖。讽刺的是 START() 注释自己都总结了「复用 sim 必须逐项复位
否则泄漏」，恰好漏了这一项。

> 修法：START() 加一行把受保护模式关掉，4 个 case 立刻复活。1 行 + 验证。

**C3. 把浮点寄存器当成整数寄存器读了**（实测坐实）
（`gaby_two_track_macros.h:545-553, 585`；触发 skip 在 `:586-589`）

背景：ARM64 有两组完全不同的寄存器——通用整数 X0–X30，向量/浮点 V0–V31（64 位浮点视图
叫 D0–D31）。**X17 和 D17 是两个物理上不同的寄存器，只是编号都叫 17。** 上游 VIXL 的
`ASSERT_EQUAL_64(期望值, 寄存器)` 有重载、认得出区别；但 gaby 重写 harness 时用了 duck
typing（「有 `GetCode()` 能返回编号就当寄存器」），**只看编号、没看是哪一组**，一律按整
数寄存器读 x[17]。于是 `ASSERT_EQUAL_64(某浮点位模式, d17)` 实际读了 x17，对不上 → harness
的「一致性预检」以为「这个期望值在我的环境不成立」→ **把整道题跳过**。

**实测坐实**：neon 那 3 个「reference sim disagrees with literal」skip 的 body，里面全是
`ASSERT_EQUAL_64(立即数, dN)`（`neon_fcvtn` 的 `d17`、`neon_modimm_movi_64bit_any` 的
`d0/d4`、`fmov_vec_imm` 的 `d0/d2`）。一致性预检在这里起了「错映射 → skip 而非假通过」
的兜底，值得肯定，但这 3 个 case 是纯损失，且 `neon_fcvtn` 里后续三条 128 位断言被连带
短路。

> 修法：识别 VRegister 走 `dreg_bits/sreg_bits`，约 20 行。

**C4. 老师只看你答错没有，从不数你跳过了几题**
（`gaby_two_track_main.h:237-243`：只在 failed>0 或 total==0 时返回非零）

把 C1–C3 连起来看就明白 C4 是结构性问题：套件只要没有「答错」的题就给绿灯，完全不关心
跳过了多少。今天 485 跑 / 104 跳；明天某改动引入类似 C2 的泄漏让 30 题悄悄从「跑」掉进
「跳」——**照样绿**。覆盖面缩水你毫不知情。C2 就是这条通道**已经真实发生过**的实例。
`tasks.md` 数字打架也源于无基线可裁决。

> 修法：每族期望 ran/skip 计数断言进 main（或提交 manifest + 比对 + 显式重基线开关），
> 计数不符即报红。约 30 行 + 用本记录第 1 节的实测数定基线。

**四个 Critical 的共同点**：测试在出问题时会「假装通过」，而激进优化最需要它别假装。合计
约一两天，每个修法都不大（C2 一行、C3 二十行）。

### Important — 应修

- **I1. FP16 断言全跳是白扔的覆盖**。gaby 完整实现了半精度浮点 leaf（`ReadHRegister`、
  `kFormatH` 链等），FP16 体在三引擎上真跑、差分有效；丢的是 116 条断言的绝对锚定 + 24
  个 case 的统计口径 + 两个多 RUN helper 体（`process_nans_half`、`fmax_fmin_h`）第一跳
  后**连差分覆盖一起丢**。这也是覆盖率里 `VisitNEON2RegMiscFP16` 零覆盖的直接原因。修复
  约 40 行（`RefStateView` 加 `hreg_bits` + 照抄 `CheckFP32` 写 `CheckFP16`）。
- **I2. tasks.md 验证数字自相矛盾**（7.2 vs 5.3/6.3）——这是 change 的合并依据，以本记录
  第 1 节实测为准统一并注明口径。
- **I3. `ASSERT_EQUAL_MEMORY` 宏注释/设计稿措辞失真**。该宏在三个已搬文件里**零使用**
  （上游用户只在未搬的 SVE 测试里），`gaby_two_track_macros.h:928-929` 的 `"(handled in
  §5)"` 指向不存在的章节；proposal「becomes a real check」字面无对象（内存语义实际由更
  强的帧窗口 oracle 兑现）。建议宏改 `((void)0)` 风格 + 如实注释。
- **I4. 帧窗口机制有两条静默退化通道且零自检**。窗口 inactive（帧指针异常 / >256KB）时帧
  复位与内存 oracle **双双静默关闭**（`gaby_two_track_macros.h:500`），幂等 store 体会无
  声失去内存校验；harness smoke 只有标量体、**一条 store 都没有**，机制有效性从未被冒烟
  验证。另：该机制与 ASan 根本不兼容（整帧 memcpy 必踩 redzone），等于焊死了给测试上
  sanitizer 的门——至少文档声明。修法：smoke 加已知布局 RMW 体自检 + inactive 时打非
  trace 可见的告警。
- **I5. 独占监视器 PRNG seed 不在 ResetState 复位面里**，ref 与 gaby 轨 seed 流在第一个
  ref-only case 后永久错位——今天靠上游 retry-loop 写法压住（`ldxr_stxr` 族实测绿），属
  「运气式 soundness」，至少记入文档。
- **I6. 夹带了与本 change 无关的改动**：`docs/refs/gaby-vm-cache-hotpath-profile-2026-06-02.md`
  追记、未跟踪的 `gaby-vm-dispatch-redesign-notes-2026-06-02.md` 与 `.html` 属 dispatch/
  bench 工作线，且追记引用了 untracked 文件（漏提交即断链）。建议拆独立 commit。
- **I7. `AssertEntryEquivalentOnce` 只验证 cache_sim 的播种等价性**，decoder_sim 三行就能
  罩住（`gaby_two_track_main.h:131-154`）。
- **I8. `ldr_literal*` 4+1 个 case 因 PredecodeCache 拒收「带字面量池的 range」整组跳过**
  ——这其实是套件揪出的**真实产品缺口**：真实 AArch64 用户态代码就是数据混在指令流里，
  cache 一拒收连 decoder 轨也被拖下水。建议产品侧把不可解码字做成 decoder-fallback 条目
  而非整段拒绝（这正是后续 dispatch 优化绕不开的输入形态）。

### Minor（择要）

- marker 约定两处形式偏离 `docs/conventions.md`（U 组自 include 剥除的单行 marker 带尾注
  文字；code-buffer 删除块未以注释保留原行）。
- `docs/testing.md` quarantine 名单漏列 `large_sim_stack`、`generic_operand`；设计稿两处
  良性偏离（core.Dump → 同址直读；scratch buffer → 帧窗口）建议补后记。
- `gaby_two_track_macros.h` 938 行超项目 800 行线，可按 engines/run/asserts/macros 四职责
  机械拆分；`kBrXzr` 在 island_smoke 里重复定义。
- 信号守护无 `sigaltstack`（栈写穿时 longjmp 可能失效）；丢了上游 test-runner 的单 case
  CLI 过滤（建议 `VIXL_PORT_ONLY=<name>` + `VIXL_PORT_FRESH_ENGINES=1` triage 开关）。
- `system_rng` 三引擎一致靠 rand seed 同步的巧合性正确，值得一行注释。

## 4. 设计评估：这套设计成立吗，有没有更优雅的走法

### 4.1 三引擎这套强在哪、天生盲区在哪

回到第 0 节那个关键事实：参考 sim 和 gaby 两轨共用同一份 leaf 语义代码，区别只在
dispatch。所以——

- 测「dispatch / cache 抄错了」：**强**（另两个不抄近路，一对露馅）。这正是本套件的主
  目标，对它是 sound 的。
- 测「某条指令算法本身写错了」：**弱**（三方一起错）。
- 唯一真正独立的真值是上游断言里**手写死的期望数字**。

这条解释了为什么 C3/C4 比表面严重：它们削弱的恰好是「手写死的期望数字」这一层——整个
系统里独立性最高的那层信号。

### 4.2 几个岔路口的选择

**拷贝 VIXL 汇编器 vs 构建时直接链 `../vixl`**：维持拷贝是对的，理由比设计稿写的更硬。
直接链有个隐患——拷来的汇编器和 gaby 手改过的共享头**必须同一个 SHA**，否则会撞上一种
「链接没报错、运行行为诡异」的 ODR（One Definition Rule：同一函数只能有一份定义，违反了
若编译器没抓到就随机用其中一份）。直接链时「隔壁是哪个 SHA」变成每台机器各自的状态，
CI / 新人最容易在这翻车；而这套件是规定每次 perf 改动必跑的关卡，给它加「你得先 checkout
对版本」的前提等于鼓励大家跳过它。2.6MB 换掉这个隐患，划算。

**自己重写断言宏 vs 最大化复用上游框架**：没走「复用上游」是对的（上游断言失败即 abort
整个进程，没法优雅转成「这条不一致」）。但重写有代价，**C3 就是代价的活样本**：上游断言
宏本来免费帮你认出「X 还是 D 寄存器」，自写的 duck typing 把这层弄丢了。教训不是「不该
重写」，而是「既然重写，就得把上游每个重载都照顾到，别假设 duck typing 自动覆盖」。

**逐条 lockstep cosim**：作主 oracle 不行（独占监视器 PRNG 会造成 mid-body 假分歧；成本
不匹配）。但 **cache vs decoder 这一对天然适合 lockstep**（同体同序同入口），且公共 API
`StepOnce/DebugStepOnce` 现成。正确形态：差分失败时**自动 lockstep 复跑该 case，报首个分
歧 PC**——把「从 FAIL 到坏缓存条目」的定位距离缩到一步（约 150–200 行）。

### 4.3 最「悬」的那个机制：帧窗口 memcpy

它在解决什么：「读改写」测试（原子 / CAS）的测试体会声明一个局部变量当缓冲区、把它的
**内存地址写死进指令**让指令去读写。三个引擎依次跑同一段代码、读写**同一块内存**——第一
个跑完把内存改脏，第二个再读就不公平了。（旧冻结方案正是死在这——它表达不了「指令里写
死真实地址」，所以把所有访存测试全扔了。）

现在的解法很巧：每个引擎开跑前，把测试函数的**整个 C++ 栈帧**用 `memcpy` 拍快照，跑完
再 `memcpy` 恢复，让下一个引擎从干净状态开始；这块快照还**顺便兼任内存对答案**
（`gaby_two_track_macros.h:339-360, 437, 500`）。

为什么「悬」：这是直接对一个正在运行的 C++ 函数栈帧做整块 memcpy，踩在未定义行为边缘。
它能工作依赖一串**没有契约级保证**的前提——编译器必须保留帧指针、局部变量必须正好落在
这个栈帧里、不能开 ASan。今天在 arm64 macOS、当前编译选项下确实能跑（-O2 与 coverage
插桩下行为一致，实测），但这些前提没有一个是白纸黑字担保的，全靠注释守护。最危险的失效
方式是**沉默**（见 I4）：帧指针读错或帧太大时，它悄悄关掉内存对答案、不报任何错。

**更优雅的替代已经躺在代码库里**：导入 simulator 有个 `MemoryWriteSink`（本来给
ShadowRunner 用）。思路换成不拍栈快照，而是**记录每个引擎往哪些地址写了什么**，跑完按
记录精确撤销，对答案比较各自的写入记录。栈帧那些脆弱假设一个都不需要了，还能覆盖堆 /
全局内存（栈快照够不着的），甚至白送越界写检测。代价只是每次内存写多一次回调，测试场景
无所谓。建议当**中期替换目标**，不用现在返工——帧窗口今天能用也快，只是它是整个设计里
唯一「靠平台惯例和注释活着、而非靠契约活着」的部件。

### 4.4 总评

骨架对、地基坚实——访存盲区从根上补上，隔离工程无可挑剔，套件快到可以随手跑。它差的不
是架构，而是「哨兵的诚实度」：几条「出了事却假装没事」的沉默通道。设计审查 **7/10**，
修完四个 Critical 后约 **8.5–9**，配得上后续的激进优化。

## 5. 行动清单（按优先级）

| 优先级 | 事项 | 工作量 |
|--------|------|--------|
| 合并前 | I2 统一 tasks.md 数字（以本记录第 1 节为准）、I3 措辞修正、I6 拆分无关改动 | 半小时 |
| **P0（当 perf 闸门前必修）** | C1 阶段感知崩溃判定 + busy-latch 修复；C2 `SetGuardedPages(false)`（1 行）；C3 寄存器 bank 分流（~20 行）；C4 计数基线（~30 行） | 合计 ~1 天 |
| P1 | I1 FP16 断言（~40 行）；I4 smoke RMW 自检 + inactive 告警；TwoTrackRun 末尾加全量绝对比对（gaby vs ref 全 RegisterFile，~50 行 + 一轮排雷）；I7 decoder_sim 播种校验；「ref 不符」类 skip 单列计数常显 | 合计 ~1–2 天 |
| P2 | FAIL 自动 lockstep 复跑 + slice hex dump（定位坏指令/坏条目）；`VIXL_PORT_ONLY`/`FRESH_ENGINES` triage 开关；I5 monitor seed 文档化；crypto 点测补进 `simulator_correctness`；I8 产品侧 data-in-stream fallback 评估 | 各半天–1 天 |
| P3/中期 | MemoryWriteSink 替换帧窗口（1–2 天）；938 行头拆分；上游宏头拷为升级 diff 对照物；评估搬 `test-simulator-aarch64.cc`（390 个穷举输入 + 1667 个硬件采集 trace，是更高一级的 leaf 真值锚，且其「同一条缓存指令反复执行」的形态恰是 predecode cache 的生产形态；3–5 天） | — |

一句话收口：**这套护栏的地基是坚实的——访存盲区真正闭合、隔离工程无可挑剔、套件快到可
以随手跑。它差的不是骨架而是「哨兵的诚实度」，四个 Critical 全部可在一两天内修完，修完
之后它配得上接下来要做的激进 dispatch 优化。在那之前，别把「`ctest -R vixl_port` 绿」
当作 perf 改动的充分放行条件。**

## 6. 索引

- 被审改动的设计稿（已对齐）：
  [`gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md`](./gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md)
  及其 OpenSpec change `openspec/changes/vixl-port-live-assemble-rewrite/`
  （`proposal` / `design` / `tasks` / `specs/aarch64-simulator/spec.md`）。
- 套件入口与边界文档：[`../testing.md`](../testing.md)（`vixl_port` 一节）、
  [`../architecture.md`](../architecture.md)（VIXL import 边界）、
  [`vixl-extraction-map.md`](./vixl-extraction-map.md)（Tier-0 test-only 拷贝、钉死 SHA
  `160c445`）、[`../../AGENTS.md`](../../AGENTS.md) 的 guard rail 条目。
- 核心实现：`test/test_support/vixl_asm/harness/gaby_two_track_macros.h`（宏 + oracle 编
  排 + 帧窗口）、`.../gaby_two_track_main.h`（隔离名单 + 崩溃守护 + 统计）、
  `.../vixl_port_oracle.{h,cc}`（比对原语）、`.../vixl_asm/CMakeLists.txt`（孤岛隔离三保
  险）；产品侧 `Sources/gaby_vm/src/gaby_vm/predecode_cache.cc`（注册拒绝与 sentinel 语义）。
