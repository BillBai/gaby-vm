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

## Chinese Communication Style

When replying in Chinese, write natural, direct prose that sounds like a real
technical conversation. Avoid AI-flavored phrasing, stiff report language,
marketing language, consulting language, and mechanical over-structuring. Do
not overuse abstract business terms. Use structure when it helps, but do not
turn every answer into a rigid list. Prefer plain wording, clear judgment, and
advice the reader can use.

Do not make Chinese prose terse just to look concise. Give enough background
for a reader who may not already know the topic. The goal is human technical
writing: neither an AI report nor a series of clipped notes.

This rule also applies to generated Chinese content, including comments, commit
messages, and explanatory text.

## Documentation Language Policy

- Write public documentation in English by default.
- If a Chinese version exists, keep it next to the English file with the
  `.zh-cn.md` extension and keep the two versions in sync when editing either
  one.
- When creating new documentation, write only the English file unless the user
  explicitly asks for a Chinese version.
- OpenSpec artifacts are English-only. Do not create bilingual OpenSpec
  documents or `.zh-cn.md` variants under `openspec/`.

## Agent Rules

- Study `../vixl` before changing related code.
- Prioritize correctness first, then interpreter execution speed. Structural changes are acceptable when they clearly serve performance or standalone maintainability.
- Guard rail before *and* after touching the shared execution hot path — decode/dispatch, the predecode cache, or imported VIXL leaf semantics — run the ported VIXL correctness suite and keep it green: `ctest --test-dir build/debug -R vixl_port` (self-contained, no `../vixl` needed). It **live-assembles** hundreds of upstream VIXL `TEST()` bodies — via a test-only assembler island (`test/test_support/vixl_asm/`, a copy of the Tier-0 VIXL assembler pinned at the import SHA) — and runs each on both tracks under a differential oracle (cache track vs decoder track) plus an absolute oracle (a VIXL reference simulator). Live assembly means load/store/ADR/literal bodies run for real against in-process memory. It exists specifically to catch a regression introduced by the predecode/dispatch optimization. Don't land perf changes on a red or un-run suite. To widen coverage or after a VIXL upgrade, re-sync the island copy at the new SHA (re-copy + update the SHA in `docs/refs/vixl-extraction-map.md`); there are no committed fixtures and no extraction tool. Details: [`docs/testing.md`](docs/testing.md#ported-vixl-tests-vixl_port).
- Do not blindly import the entire VIXL repository. Stay within the import tiers defined in [`docs/refs/vixl-extraction-map.md`](docs/refs/vixl-extraction-map.md).
- Do not invent a new IR.
- Do not add JIT assumptions: no runtime code generation, no RWX memory, no executable memory allocation. Predecoded data is ordinary data.
- Do not rewrite simulator leaf semantics just to make the cache cleaner.
- Edits to imported files must use the marker convention; see [`docs/architecture.md`](docs/architecture.md#vixl-import-boundary).
- Keep CMake working.
- Keep iOS/macOS/POSIX portability in mind. Avoid platform assumptions that would make iOS embedding difficult.
- Preserve upstream license headers; never rewrite VIXL copyrights.
- Optimize with measurements, but do not avoid necessary architectural changes merely to keep patches small.
- Measure perf with the benchmark harness, don't eyeball it: `bench/` holds developer-invoked benchmarks (run directly, not via `ctest`, which is for correctness). The representative slowdown-vs-native number is `bench_business` — five iOS-business-logic microkernels (`parse`/`hash`/`struct`/`fsm` scalar + `applogic`, the only FP/NEON one) — run `--mode cache` against `--mode decoder` and the `native_business` host-CPU baseline, reported per shape. Build behind `-DGABY_VM_BUILD_BENCHMARKS=ON` (add `-DGABY_VM_BUILD_NATIVE_BASELINE=ON` for the native denominator; arm64 host only). Run `bench_business --verify` (cache==decoder bit-check) after any leaf or kernel change, and quote a before/after from this harness when a change targets execution speed. Details: [`bench/README.md`](bench/README.md).

## References

Durable conventions and design facts live in `docs/`. Read these before proposing or making non-trivial changes:

- Architecture, memory model, threading model, VIXL import boundary → [`docs/architecture.md`](docs/architecture.md)
- Internal build structure (targets, warning-policy split, VIXL define scoping) → [`docs/build.md`](docs/build.md)
- Coding conventions (formatting, namespaces, license headers, marker convention) → [`docs/conventions.md`](docs/conventions.md)
- Testing strategy (CTest layout, the ported VIXL correctness guard rail, encoding policy) → [`docs/testing.md`](docs/testing.md)
- Performance measurement (benchmark harness, slowdown-vs-native methodology, the business-logic microkernels) → [`bench/README.md`](bench/README.md)
- Continuous integration (GitHub Actions workflows, the portable `ci/` scripts, size/bench reports) → [`docs/refs/ci.md`](docs/refs/ci.md)
- VIXL import tier list → [`docs/refs/vixl-extraction-map.md`](docs/refs/vixl-extraction-map.md)
- Capability requirements (normative) → [`openspec/specs/`](openspec/specs/)
- User-facing build and embedding instructions → [`README.md`](README.md)

**Doc paths drift.** Files under `docs/` get periodically archived and reorganized (see [`docs/refs/README.md`](docs/refs/README.md)), so a path cited in code or another doc may go stale. If a referenced file isn't where the link points, search by filename — filenames are stable, paths are not. Don't spend effort maintaining exact doc paths unless explicitly asked to.
