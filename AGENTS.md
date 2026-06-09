# Gaby-VM

> **Note:** `CLAUDE.md` in this repository is a symbolic link to this file (`AGENTS.md`). Both paths resolve to the same content — edit `AGENTS.md` and Claude Code, along with any other agent that reads `CLAUDE.md`, will see the change automatically. Do not edit `CLAUDE.md` directly; do not duplicate its contents.

## Context

This is a standalone project, not an in-place fork of VIXL.

The reference VIXL source tree is expected at:

```text
../vixl
```

Use `../vixl` to study behavior and dependencies. Copy only the VIXL AArch64 simulator code and required dependencies into this repository. Preserve license headers and attribution.

## Goal

Build an embeddable Arm CPU instruction interpreter based on VIXL simulator semantics, starting with AArch64 user-mode execution.

The main optimization direction is simple:

```text
predecode once -> cache decoded dispatch target -> execute cached path repeatedly
```

This is a decode/dispatch cache, not a new complex IR. Keep VIXL's leaf execution code wherever practical; change the repeated execution path, not the semantics.

The primary no-JIT target is iOS. The project should also support macOS and POSIX-like environments such as Linux, Android, and HarmonyOS. Windows is not a target for now.

Correctness first, then interpreter execution speed.

## Scope

V1 focuses on EL0/user-mode A64 execution, ordinary register/memory behavior, and compatibility with the imported VIXL simulator semantics.

Out of scope unless explicitly required later:

- MMU
- device model
- kernel/hypervisor/firmware execution
- full EL1/EL2/EL3 exception model
- full hardware/system emulator behavior
- JIT code generation
- executable memory allocation
- QEMU/TCG-style translation layer

Traps, unsupported instructions, syscall-like instructions, or simulated faults may be represented later through callbacks, status codes, or fallback behavior. Do not build a full exception-level simulator in V1.

## 中文表达风格

用中文回复时，请用自然、清醒、像真人聊天的中文。避免 AI 味、报告腔、公众号腔、咨询腔和过度结构化。不要滥用「本质、底层逻辑、维度、框架、闭环、赋能、抓手、范式」这类抽象词。可以有结构，但不要机械分点。优先说人话、说重点、给判断、给能直接用的建议。不要过度安慰，不要灌鸡汤，不要为了显得全面而啰嗦。

同时也不要为了简洁而把句子砍得过短。要把背景介绍清楚，详细一点，假设读者不一定具备相关知识。整体目标是「说人话」——既不是 AI 报告体，也不是电报式短句。

这条规则对生成的文档内容同样适用（包括中文注释、中文 commit message、中文说明性文本等等）。

## Agent Rules

- Study `../vixl` before changing related code.
- Prioritize correctness first, then interpreter execution speed. Structural changes are acceptable when they clearly serve performance or standalone maintainability.
- Guard rail before *and* after touching the shared execution hot path — decode/dispatch, the predecode cache, or imported VIXL leaf semantics — run the ported VIXL correctness suite and keep it green: `ctest --test-dir build/debug -R vixl_port` (self-contained, no `../vixl` needed). It replays hundreds of upstream VIXL `TEST()` bodies on both tracks under a differential oracle (cache track vs decoder track) plus an absolute oracle, and exists specifically to catch a regression introduced by the predecode/dispatch optimization. Don't land perf changes on a red or un-run suite. To widen coverage or after a VIXL upgrade, regenerate the committed fixtures (the extraction tool is dev-only, behind `-DGABY_VM_BUILD_VIXL_EXTRACT=ON`). Details: [`docs/testing.md`](docs/testing.md#ported-vixl-tests-vixl_port).
- Do not blindly import the entire VIXL repository. Stay within the import tiers defined in [`docs/refs/vixl-extraction-map.md`](docs/refs/vixl-extraction-map.md).
- Do not invent a new IR.
- Do not add JIT assumptions: no runtime code generation, no RWX memory, no executable memory allocation. Predecoded data is ordinary data.
- Do not rewrite simulator leaf semantics just to make the cache cleaner.
- Edits to imported files must use the marker convention; see [`docs/architecture.md`](docs/architecture.md#vixl-import-boundary).
- Keep CMake working.
- Keep iOS/macOS/POSIX portability in mind. Avoid platform assumptions that would make iOS embedding difficult.
- Preserve upstream license headers; never rewrite VIXL copyrights.
- Optimize with measurements, but do not avoid necessary architectural changes merely to keep patches small.

## References

Durable conventions and design facts live in `docs/`. Read these before proposing or making non-trivial changes:

- Architecture, memory model, threading model, VIXL import boundary → [`docs/architecture.md`](docs/architecture.md)
- Internal build structure (targets, warning-policy split, VIXL define scoping) → [`docs/build.md`](docs/build.md)
- Coding conventions (formatting, namespaces, license headers, marker convention) → [`docs/conventions.md`](docs/conventions.md)
- Testing strategy (CTest layout, the ported VIXL correctness guard rail, encoding policy) → [`docs/testing.md`](docs/testing.md)
- VIXL import tier list → [`docs/refs/vixl-extraction-map.md`](docs/refs/vixl-extraction-map.md)
- Capability requirements (normative) → [`openspec/specs/`](openspec/specs/)
- User-facing build and embedding instructions → [`README.md`](README.md)
