# VIXL Overview (for Gaby-VM contributors)

> Reference notes about the upstream
> [VIXL](https://github.com/Linaro/vixl) project as it lives at `../vixl`
> from this repository. Gaby-VM imports a subset of VIXL's AArch64
> simulator semantics; this document explains what VIXL is, which parts
> Gaby-VM cares about, which parts it does not, and how upstream is
> built and tested. **Citations are file paths inside `../vixl/` unless
> stated otherwise.**

## What VIXL is

VIXL is a permissively licensed (BSD 3-Clause) C++17 library from Arm
that bundles four loosely-coupled subsystems:

- **An assembler / macro-assembler** for AArch32 (A32+T32) and AArch64
  (A64). These emit instruction bytes into a code buffer at runtime.
- **A disassembler** that pretty-prints instruction bytes.
- **A decoder** that classifies instruction bytes by form and dispatches
  to a visitor.
- **An instruction-set simulator** for AArch64 user-mode execution. The
  simulator is what Gaby-VM is being built around. There is no AArch32
  simulator in upstream.

Plus shared support: CPU feature flags, host-platform abstractions,
arithmetic / bit utilities, code-buffer allocation, pool managers, and
test/benchmark scaffolding.

The upstream entry point for orientation is `../vixl/README.md`. Build
status, supported toolchains, and a high-level "what is in here" summary
live there.

## Top-level layout

```
../vixl/
  src/                    shared root + per-ISA subdirs
    aarch32/              AArch32 assembler/disassembler/macro-assembler
    aarch64/              AArch64 simulator + assembler/disassembler/...
    *.h, *.cc             shared (utils, code-buffer, cpu-features, ...)
  test/
    aarch32/              AArch32 tests
    aarch64/              AArch64 tests (assembler, simulator, ...)
    test-runner-*.cc      cctest harness
  examples/
    aarch32/, aarch64/    example programs
  benchmarks/
    aarch32/, aarch64/    micro-benchmarks
  doc/
    aarch32/, aarch64/    per-ISA topic docs (debugger, trace, ...)
    changelog.md
  tools/                  Python build/lint/test scripts
  SConstruct              SCons build entry point
  LICENCE                 BSD 3-Clause (note: British spelling)
  README.md               high-level overview
```

`src/aarch64/` contains 31 files: simulator, decoder, instruction
helpers, disassembler, debugger, CPU-feature auditor, assembler family,
pointer auth, register/operand abstractions. Most of what Gaby-VM
imports lives here.

## Subset relevant to Gaby-VM

Gaby-VM is a *consumer* of pre-existing instruction bytes — it does
not generate code. So most assembler/macro-assembler files are out of
scope. The relevant tiers (the
[`vixl-extraction-map.md`](./vixl-extraction-map.md) doc enumerates them
in detail):

- **Simulator core.** `src/aarch64/simulator-aarch64.{h,cc}`,
  `src/aarch64/simulator-constants-aarch64.h`,
  `src/aarch64/logic-aarch64.cc`. The execution engine and its
  instruction leaves (NEON / SVE arithmetic helpers, etc.).
- **Decoder + instruction-form metadata.**
  `src/aarch64/decoder-aarch64.{h,cc}`,
  `src/aarch64/decoder-constants-aarch64.h`,
  `src/aarch64/decoder-visitor-map-aarch64.h`,
  `src/aarch64/instructions-aarch64.{h,cc}`,
  `src/aarch64/constants-aarch64.h`. The decoder picks a "form" for
  each 32-bit instruction word and drives the visitor chain. The
  per-instruction control flow is described in
  [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md).
- **Auditor (effectively required).**
  `src/aarch64/cpu-features-auditor-aarch64.{h,cc}`. The Simulator
  asserts on it inside `ExecuteInstruction()` (`simulator-aarch64.h:1441`),
  so dropping it requires editing that assertion. Gaby-VM keeps it on.
- **Disassembler (recommended).** `src/aarch64/disasm-aarch64.{h,cc}`.
  The Simulator constructs a `PrintDisassembler` in its constructor
  (`simulator-aarch64.cc:666`); used only when trace flags are
  non-zero, but stubbing it requires constructor edits.
- **Registers, operands, ABI.** `src/aarch64/registers-aarch64.{h,cc}`,
  `src/aarch64/operands-aarch64.{h,cc}`, `src/aarch64/abi-aarch64.h`.
- **CPU model.** `src/aarch64/cpu-aarch64.{h,cc}` — capability queries.
- **Pointer auth.** `src/aarch64/pointer-auth-aarch64.{h,cc}` — used by
  the Simulator for PAC instruction support.
- **Shared root.** `src/globals-vixl.h`, `src/platform-vixl.h`,
  `src/utils-vixl.{h,cc}`,
  `src/compiler-intrinsics-vixl.{h,cc}`, `src/cpu-features.{h,cc}`.

These together form the "import surface" for Gaby-VM V1.

## Subset Gaby-VM does *not* import (V1)

These are upstream parts that Gaby-VM has no use for in V1 because
they are about *generating* code, not *interpreting* it.

- **Assembler / macro-assembler.** `src/aarch64/assembler-aarch64.{h,cc}`,
  `src/aarch64/assembler-sve-aarch64.cc`,
  `src/aarch64/macro-assembler-aarch64.{h,cc}`,
  `src/aarch64/macro-assembler-sve-aarch64.cc`,
  `src/assembler-base-vixl.h`, `src/code-generation-scopes-vixl.h`,
  `src/code-buffer-vixl.{h,cc}`, `src/pool-manager.h`,
  `src/pool-manager-impl.h`, `src/macro-assembler-interface.h`,
  `src/invalset-vixl.h`. Reconsider only if the benchmark harness needs
  runtime assembly.
- **Debugger.** `src/aarch64/debugger-aarch64.{h,cc}`. The Simulator
  constructs one but disables it by default
  (`simulator-aarch64.cc:704-705`). Defer until a debugger workflow is
  needed; either stub the type for the constructor or import the file.
- **All AArch32.** `src/aarch32/`, `test/aarch32/`, `examples/aarch32/`,
  `benchmarks/aarch32/`, `doc/aarch32/`. Gaby-VM is AArch64-only.

## Build and test system (upstream)

VIXL ships with SCons. Gaby-VM uses CMake, so the upstream build
infrastructure is reference-only — but the SCons targets and defines are
the canonical way to obtain a baseline binary for benchmarking.

Common SCons invocations from `../vixl/`:

```bash
# Library only.
scons simulator=aarch64 mode=release -j$(nproc)

# Library + AArch64 examples.
scons simulator=aarch64 mode=release aarch64_examples

# Library + AArch64 simulator benchmarks.
scons simulator=aarch64 mode=release aarch64_benchmarks

# Tests (produces test-runner binary).
scons simulator=aarch64 mode=release tests

# Everything.
scons simulator=aarch64 mode=release all
```

Significant build flags:

- `simulator=aarch64` — compile in the AArch64 simulator
  (`VIXL_INCLUDE_SIMULATOR_AARCH64`).
- `target=a64` — set the AArch64 ISA target (`VIXL_INCLUDE_TARGET_A64`).
  Auto-detected on most hosts.
- `mode=release` / `mode=debug` — `VIXL_DEBUG` only set for `debug`.
- `VIXL_CODE_BUFFER_MMAP` / `VIXL_CODE_BUFFER_MALLOC` — code-buffer
  allocator. Irrelevant to Gaby-VM (we exclude the code buffer).
- `compiler=gcc|clang`, `std=c++17` — toolchain selectors.

The `tools/` directory wraps SCons for development:

- `tools/test.py` — runs full test matrix in debug + release.
- `tools/test_runner.py` — discovers and filters tests in `test-runner`
  via `--list` / `--run_test <regex>`.
- `tools/clang_format.py`, `tools/clang_tidy.py` — lint runners.
- `tools/generate_simulator_traces.py` — regenerates simulator-trace
  test inputs.

The test runner executable (built by `scons … tests`) supports:

```bash
./obj/.../test-runner --list                       # all test names
./obj/.../test-runner --list | grep -i sim         # simulator tests
./obj/.../test-runner --run_test 'AARCH64_SIM_.*'  # run a subset
```

Operational details for running upstream as a baseline live in
[`baseline-benchmark-suite.md`](./baseline-benchmark-suite.md).

## License and attribution

VIXL is BSD 3-Clause. The license file at the upstream root is
`LICENCE` (British spelling — note the absence of the trailing `S`). The
canonical per-file header looks like (verbatim from the top of
`src/aarch64/simulator-aarch64.cc`):

```
// Copyright 2015, VIXL authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
// ... (warranty disclaimer continues for ~10 lines) ...
```

Gaby-VM rules (from `docs/README.md`):

- **Imported VIXL files keep their upstream header verbatim.** Do not
  rewrite the year, the copyright owner, or the warranty text.
- **New Gaby-VM files use the Gaby-VM 2026 BSD-3 header** shown in
  `docs/README.md`.
- Add an upstream attribution line in `AUTHORS` covering the imported
  scope when import lands.

## Host-platform notes (relevant to iOS embedding)

The Simulator does not allocate executable memory. It does, however,
include host-platform pieces that need attention on iOS:

- `src/platform-vixl.h::HostBreakpoint()` raises `SIGINT`. Acceptable
  on POSIX; consider stubbing on iOS app builds, where `signal()`
  delivery is restricted.
- `src/aarch64/simulator-aarch64.cc:640-643` contains an inline x86_64
  `__asm__` block guarded by `VIXL_ENABLE_IMPLICIT_CHECKS`. Leave the
  macro undefined on iOS; the path is for `pipe`-based memory
  probing, not execution.
- `src/code-buffer-vixl.cc` uses `mmap` + `mprotect(PROT_EXEC)`. Gaby-VM
  excludes this file entirely (no code generation), so iOS's W^X
  enforcement is not a concern for the import surface.

## Where to read next

- [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md)
  — the moving parts of the Simulator class and its ancillary objects.
- [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md)
  — the per-instruction control flow that the Gaby-VM predecode cache
  short-circuits.
- [`vixl-extraction-map.md`](./vixl-extraction-map.md) — exact file
  list and dependency notes for the import.
- [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
  — the Gaby-VM-specific design (predecode cache, multi-instance
  concurrency, real atomic semantics, embedder-allocated stacks).
- [`baseline-benchmark-suite.md`](./baseline-benchmark-suite.md) — how
  to build upstream VIXL, run its tests, and measure baseline
  simulator throughput.
