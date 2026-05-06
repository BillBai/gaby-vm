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

This is a decode/dispatch cache, not a new complex IR.

The primary no-JIT target is iOS. The project should also support macOS and POSIX-like environments such as Linux, Android, and HarmonyOS. Windows is not a target for now.

A core goal is to make interpreted execution as fast as practical while preserving correctness.

## Scope

V1 should focus on EL0/user-mode A64 execution, ordinary register/memory behavior, and compatibility with the imported VIXL simulator semantics.

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

## Implementation Direction

Keep VIXL's AArch64 simulator leaf execution code wherever practical.

Change the repeated execution path from:

```text
decode instruction -> visitor dispatch -> execute leaf
```

to:

```text
cached instruction -> direct cached leaf dispatch
```

Do not rewrite instruction semantics just to make the cache cleaner.

## Build and Platform Notes

Use CMake for this standalone repository.

Keep the code portable across iOS, macOS, and POSIX-like systems. Avoid platform assumptions that would make iOS embedding difficult.

No runtime code generation:

- no JIT
- no RWX memory
- no executable memory allocation
- predecoded data is ordinary data

## Validation

Use VIXL's own tests where practical.

Also add Stretto-specific tests for:

- standalone extraction correctness
- baseline simulator behavior
- predecoded dispatch behavior
- fallback behavior
- performance benchmarks

When possible, compare the cached path against the original VIXL-style simulator path.

## Agent Rules

- Study `../vixl` before changing related code.
- Prioritize correctness first, then interpreter execution speed. Structural changes are acceptable when they clearly serve performance or standalone maintainability.
- Do not blindly import the entire VIXL repository.
- Do not invent a new IR unless explicitly asked.
- Do not add JIT assumptions.
- Do not rewrite simulator leaf semantics unnecessarily.
- Keep CMake working.
- Keep iOS/macOS/POSIX portability in mind.
- Preserve upstream license headers.
- Optimize with measurements, but do not avoid necessary architectural changes merely to keep patches small.
