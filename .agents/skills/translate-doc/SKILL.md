---
name: translate-doc
description: Use when the user wants a document, file, README, spec, or web page translated into Chinese (e.g. "翻译 xxx 文件", "translate this doc", "把这篇翻成中文", "读英文慢帮我翻一下", "翻译一下 docs/architecture.md"). Applies to spec-driven projects with lots of English design docs.
---

# Translate Doc

## Overview

把英文文档翻译成自然、流畅的中文，结果**直接输出到对话里**供阅读。**绝不修改原文件。** 目标读者是中文母语开发者，不是初学者——要说人话，不要 AI 报告腔。

适用场景：spec-driven 项目里堆了一堆英文 design doc / proposal / spec / RFC，用户读英文慢，想看中文版但不想污染仓库。

## When to Use

- 用户说「翻译 xxx 文件」「translate this doc」「把这篇翻成中文」「读英文慢帮我翻一下」
- 用户读完英文文档后说「能不能给个中文版」
- 用户在 OpenSpec / RFC / design doc workflow 里想快速理解英文 proposal

**Don't use when:**

- 用户要求**修改/重写**原文件 → 那是 edit 任务，不是这个 skill
- 用户只想要文档**大意/总结** → 直接概括就行，不要逐段翻
- 当前对话用英文进行 → 输出语言跟着对话语种走

## Output Rules

1. **默认只输出到对话**——不写文件、不动原文件、不创建副本。
2. **只输出中文**：默认直接给出中文译文，不做中英对照、不重复原文段落。要看原文用户自己回去翻文件。仅当用户**明确**要求「中英对照」「双语」「保留原文」时，才并列输出。
3. **可选写文件**：仅当用户**明确**说「写到 `xxx.zh.md`」「保存成中文版」「生成一份中文文件」时，才创建并列的 `.zh.md` 文件（例如 `docs/architecture.md` → `docs/architecture.zh.md`）。**原文件永远不动。**
4. **完整翻译**：除非用户指定段落或章节，否则全文翻译，不要擅自总结、删节、重排。
5. **保留结构**：Markdown 标题层级、列表、表格、代码块结构原样保留，只翻译里面的自然语言文本。

## Preservation Rules

以下内容**保持英文原样**，不翻译：

| 类型 | 例子 |
|------|------|
| 项目专有术语 | simulator, predecode, dispatch cache, AArch64, ELx, VIXL, opcode |
| 通用技术名词 | cache, register, syscall, callback, runtime, hook, trap |
| 代码块 / inline code | `` `git status` ``, `` `predecode_cache.cpp` ``, `int main()` |
| 文件路径、命令、URL | `docs/architecture.md`, `cmake --build`, `https://...` |
| 标题/链接里的 anchor ID | `#vixl-import-boundary`（翻译会破坏跳转锚点） |
| 缩写、品牌、版本号 | iOS, macOS, ARM, Cortex-A78, ARMv8.6-A, EL0 |
| OpenSpec/RFC 关键字 | spec, proposal, delta, capability, requirement |

**拿不准的术语，宁可保留英文。** 第一次出现时可以用「中文释义（English）」格式给一次提示，之后只用英文。比如：「预解码缓存（predecode cache）」首次出现，后文都写 `predecode cache`。

## Chinese Style

**最高优先级**：当前项目根目录的 `CLAUDE.md` / `AGENTS.md` 里如果有「中文表达风格」「Chinese style」之类的章节，**完全按那个走**。该章节是项目主人定的最终标尺。

如果项目没定，套以下默认（与多数 spec-driven 项目兼容）：

- **说人话**。自然、清醒、像真人在 IM 里讲话。避免 AI 味、报告腔、公众号腔、咨询腔。
- **不要滥用抽象词**。「本质」「底层逻辑」「维度」「框架」「闭环」「赋能」「抓手」「范式」基本不用。技术词照常用，比如「调度」「缓存」「指令」没问题。
- **不要机械分点**。可以有结构，但别为了显得全面而把每句话都切成 bullet。能用一段话讲清楚的就用一段。
- **不要砍得太短**。要把背景介绍清楚，详细一点，假设读者不一定具备相关知识。目标是「说人话」——既不是 AI 报告体，也不是电报式短句。
- **保留原文语气**。吐槽就翻成吐槽，正式就翻成正式，注释里的 `TODO`/`HACK`/`XXX` 不要美化或删除。
- **被动语态转主动**。英文里的 "X is done by Y" 翻成「Y 做 X」更自然。
- **代词补足**。英文 "it/they" 含糊时，中文要把指代对象写清楚。

## Quick Reference

执行流程：

1. 用 `Read` 工具读原文件。**不要 Write，不要 Edit。**
2. 扫一遍，识别保留项：项目术语、代码、路径、anchor、缩写。
3. 按段翻译。Markdown 结构原样保留。
4. 输出到对话——只输出中文译文，不做中英对照（除非用户明确要求双语）。
5. 用户**明确**要存文件，再写 `<原文件名>.zh.md`，绝不覆盖原文件。

## Common Mistakes

- ❌ 直接 `Edit` 或 `Write` 原文件 → ✅ 只 `Read`，结果输出到对话
- ❌ 把 `predecode cache` 翻成「预解码缓存」反复出现 → ✅ 首次「预解码缓存（predecode cache）」，之后只用 `predecode cache`
- ❌ 翻译 anchor `#some-section` → ✅ 锚点 ID 一律保留原样
- ❌ 套 AI 报告体（「综上所述」「本质上」「从…维度看」「赋能」「闭环」） → ✅ 说人话
- ❌ 用户没要求就主动生成 `.zh.md` 文件 → ✅ 默认只输出到对话
- ❌ 把 `iOS` 写成「iOS 系统」、把 `cache` 写成「高速缓存」 → ✅ 缩写和通用术语保留英文
- ❌ 自作主张总结/精简 → ✅ 完整翻译，结构对齐
- ❌ 用户没要求就做中英对照、贴一段英文原文再贴中文 → ✅ 默认只输出中文译文
- ❌ 把 commit message / TODO 注释里的吐槽美化 → ✅ 保留原语气

## Red Flags — STOP and Reconsider

看到自己在做下面这些事就停下：

- 准备 `Edit` 原文件 → 立刻取消，改用 `Read` + 对话输出
- 准备直接生成 `.zh.md` 而用户没说要存文件 → 先确认
- 翻译里出现「赋能」「抓手」「闭环」「底层逻辑」 → 重写那一段
- 把代码块里的英文也翻了 → 还原代码块
- 输出里贴了整段英文原文做对照而用户没要求 → 删掉英文，只留中文
- 文档很长就「先翻一半」 → 要么完整翻完，要么明确告诉用户「太长，先翻 A 节，要继续告诉我」
