# VIXL Extraction Map

> Concrete file list for importing the VIXL AArch64 simulator into
> Gaby-VM, organized by tier (must-have / recommended / deferrable /
> out). Citations are paths inside `../vixl/` (e.g. `src/aarch64/…`).
> The imported subset (Tiers 1–3) now lives in this repository under
> `Sources/gaby_vm/src/…`, mirroring that same relative structure — so a
> `src/aarch64/foo.cc` citation below resolves to
> `Sources/gaby_vm/src/aarch64/foo.cc` here. Tier 0 files are cited for
> reference only and are not imported.
>
> This is a **reference map**, not a task list. The actual import
> change will use these tiers to scope its file list, dependencies,
> and CMake updates.

## Tier 1 — must-have for the V1 import

These are required for a runnable AArch64 user-mode simulator.

### Shared root (under `src/`)

| File | Why |
|------|-----|
| `src/globals-vixl.h` | C++17 platform/host detection, `VIXL_ASSERT`/`VIXL_CHECK`, `VIXL_HOST_POINTER_*`, abort macros. |
| `src/platform-vixl.h` | `HostBreakpoint()` (`raise(SIGINT)`). Trivial header. |
| `src/utils-vixl.h` / `.cc` | Bit manipulation, integer/FP helpers, byte swaps, alignment, `Hash()` used by the form-hash dispatch. |
| `src/compiler-intrinsics-vixl.h` / `.cc` | Compiler builtin wrappers (popcount, clz, ctz, etc.). |
| `src/cpu-features.h` / `.cc` | `CPUFeatures` bitset (`kSVE`, `kSVE2`, `kPAuth`, `kMTE`, `kBTI`, …). Used everywhere. |

### AArch64 simulator core (under `src/aarch64/`)

| File | Why |
|------|-----|
| `simulator-aarch64.h` / `.cc` | The Simulator class, register state, `ExecuteInstruction`, exclusive monitors, branch interception, runtime calls. |
| `simulator-constants-aarch64.h` | Stack sizes, FPCR layouts, sim limits. |
| `logic-aarch64.cc` | NEON / SVE arithmetic implementations called by the simulator. |

### Decoder + instruction-form metadata

| File | Why |
|------|-----|
| `decoder-aarch64.h` / `.cc` | `Decoder`, `DecodeNode`, `CompiledDecodeNode`, `VisitNamedInstruction`, visitor-list management. |
| `decoder-constants-aarch64.h` | `kDecodeMapping[]` — the data that drives `ConstructDecodeGraph()`. |
| `decoder-visitor-map-aarch64.h` | The `DEFAULT_FORM_TO_VISITOR_MAP` and `SIM_AUD_VISITOR_MAP` macros. |
| `instructions-aarch64.h` / `.cc` | `Instruction` (zero-byte wrapper over instruction bytes), bit-field accessors, sampling helpers. |
| `constants-aarch64.h` | Register codes (`kSpRegCode = 31`, etc.), opcode tables, immediate ranges. |

### Auditor (effectively required)

| File | Why |
|------|-----|
| `cpu-features-auditor-aarch64.h` / `.cc` | The Simulator's `ExecuteInstruction()` asserts on `cpu_features_auditor_.InstructionIsAvailable()` (`simulator-aarch64.h:1441`). Removing the assertion is a Gaby-VM-local change we should avoid. |

### Registers / operands / ABI

| File | Why |
|------|-----|
| `registers-aarch64.h` / `.cc` | `CPURegister`, register codes, formatting. |
| `operands-aarch64.h` / `.cc` | `Operand`, `MemOperand` — read by visitor leaves. |
| `abi-aarch64.h` | Calling-convention metadata used by the templated `RunFrom<R, P...>` and `DoRuntimeCall<R, P...>` paths. |

### CPU model

| File | Why |
|------|-----|
| `cpu-aarch64.h` / `.cc` | Lightweight CPU capability queries; the simulator references it for some instruction availability decisions. Note: `cpu-aarch64.h:47` `#error`s if `VIXL_INCLUDE_TARGET_AARCH64` is undefined, so the CMake config must define it. |

### Pointer authentication

| File | Why |
|------|-----|
| `pointer-auth-aarch64.cc` | Simulator member function definitions for PAC keys and PAC operations. **There is no matching `.h`** — declarations live inside `simulator-aarch64.h`. So we import only the `.cc`. |

## Tier 2 — recommended for a usable build

These are not strictly required to *run* code, but the Simulator's
header pulls them in and the constructor unconditionally creates one,
so excluding them requires editing imported files.

| File | Why |
|------|-----|
| `disasm-aarch64.h` / `.cc` | `Disassembler`, `PrintDisassembler`. Simulator constructs `print_disasm_ = new PrintDisassembler(stream_);` at `simulator-aarch64.cc:666`, and `simulator-aarch64.h` includes `disasm-aarch64.h` directly. Stubbing requires header edits. Tier-2 reflects this. Used by trace. |

## Tier 3 — deferrable

These are optional to V1 but trivial enough to import that we should
not waste energy stubbing them out.

| File | Why |
|------|-----|
| `debugger-aarch64.h` / `.cc` | `Debugger`. Constructed and disabled by default (`simulator-aarch64.cc:704-705`); `simulator-aarch64.h` includes the header. Stubbing requires header edits — easier to import. |

## Tier 0 — out of scope (V1)

Do not import these unless a downstream need appears. Rationale:
Gaby-VM is a *consumer* of pre-existing instruction bytes; it does not
generate code.

> **Pinned import SHA — `160c445`** (`../vixl` HEAD, 2026-05-14, "Port build
> and tests to macOS (arm64)"). The imported Tier 1–3 files above were taken
> at this commit, and a **test-only copy** of selected Tier 0 assembler files
> (see [`gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md`](./gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md))
> is pinned to the *same* SHA. That copy lives under
> **`test/test_support/vixl_asm/`** — it is used by the `vixl_port` suite to
> live-assemble upstream VIXL test bodies — and is **excluded from `gaby_vm`**:
> it compiles into a `gaby_vm_vixl_asm_testonly` library (no `::` alias,
> PRIVATE-linked, gated on `GABY_VM_BUILD_TESTS`) that is never part of the
> shipping build. No Tier 0 file appears under `Sources/gaby_vm/src/`. This pin
> is load-bearing, not bookkeeping: the
> copied assembler `.cc` files are written against upstream headers but compile
> against gaby's hand-edited leaf headers (e.g. the constexpr-inline
> `VectorFormat` helpers), so a SHA skew between the two halves could trigger a
> **no-diagnostic inline-body ODR** — it would link clean and misbehave at
> runtime. Re-sync both halves together (`git -C ../vixl checkout <new-sha>`,
> re-copy, update this line) on any future VIXL upgrade.

### Code-generation (assembler, macro-assembler, code buffer, pools)

- `src/aarch64/assembler-aarch64.h` / `.cc`
- `src/aarch64/assembler-sve-aarch64.cc`
- `src/aarch64/macro-assembler-aarch64.h` / `.cc`
- `src/aarch64/macro-assembler-sve-aarch64.cc`
- `src/assembler-base-vixl.h`
- `src/code-generation-scopes-vixl.h`
- `src/code-buffer-vixl.h` / `.cc` — uses `mmap` + `mprotect(PROT_EXEC)`,
  which is W^X-incompatible on iOS. Not needed since we are not
  emitting machine code.
- `src/pool-manager.h` / `pool-manager-impl.h`
- `src/macro-assembler-interface.h`
- `src/invalset-vixl.h` (used by pool-manager and assembler internals)

### AArch32 (entire ISA)

- `src/aarch32/` (assembler, macro-assembler, instructions,
  disassembler, operands, location, constants)
- `test/aarch32/`
- `examples/aarch32/`
- `benchmarks/aarch32/`
- `doc/aarch32/`

### Build & tooling

- `SConstruct` (Gaby-VM uses CMake; SConstruct is reference for
  baseline builds, not for import).
- `tools/*.py` (Python build/lint scripts; useful as documentation
  for upstream test workflows).

## Header-include topology (cross-references between Tier 1 files)

Drawn from `#include` lines at the top of each header and `.cc`:

```
simulator-aarch64.h
  -> abi-aarch64.h
  -> cpu-features-auditor-aarch64.h
  -> debugger-aarch64.h           (Tier 3 — pulled in by header)
  -> disasm-aarch64.h             (Tier 2 — pulled in by header)
  -> instructions-aarch64.h
  -> simulator-constants-aarch64.h
  -> ../cpu-features.h
  -> ../globals-vixl.h
  -> ../utils-vixl.h
  + std: <memory> <mutex> <random> <unordered_map> <vector> <optional>
         <functional> <iostream> <sstream> <type_traits> <unordered_set>

decoder-aarch64.h
  -> instructions-aarch64.h
  -> ../globals-vixl.h
  + std: <list> <map> <string> <unordered_map> <unordered_set>

decoder-aarch64.cc
  -> decoder-aarch64.h
  -> decoder-constants-aarch64.h
  -> ../globals-vixl.h
  -> ../utils-vixl.h

instructions-aarch64.h
  -> constants-aarch64.h
  -> ../globals-vixl.h
  -> ../utils-vixl.h

cpu-aarch64.h
  -> simulator-aarch64.h          (the only file that brings in simulator)
  -> instructions-aarch64.h
  -> ../cpu-features.h
  -> ../globals-vixl.h
  + #error if !VIXL_INCLUDE_TARGET_AARCH64

simulator-aarch64.cc
  -> simulator-aarch64.h
  -> ... + <sys/mman.h> <unistd.h> on POSIX
        + <Windows.h> <Memoryapi.h> on Windows (#ifdef'd)
        + <errno.h>
```

Implications:

1. **Tier 2/3 cannot be skipped without header edits.** The Simulator
   header includes both `disasm-aarch64.h` and `debugger-aarch64.h`
   directly. We import them.
2. **`cpu-aarch64.h` requires `VIXL_INCLUDE_TARGET_AARCH64`.** Set this
   in the Gaby-VM CMake target.
3. **`simulator-aarch64.h` includes `<mutex>`** — a thread-safety
   primitive lurks somewhere in the Simulator (used at least by
   `MetaDataDepot` for MTE tag-map locks). Import unchanged; verify
   per-instance ownership during the multi-instance audit (see
   [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)).
4. **`simulator-aarch64.cc` pulls `<sys/mman.h>` and `<unistd.h>`** for
   the POSIX implicit-checks pipe. iOS implication: keep the Windows
   include path inactive (it is); make sure `pipe()` is available on
   the target. The path is only compiled when
   `VIXL_ENABLE_IMPLICIT_CHECKS` is defined; default is off.

## License and attribution

VIXL is BSD 3-Clause. The license file at the upstream root is
`LICENCE` (British spelling — note the absence of the trailing `S`).
Each upstream file carries a per-file header beginning with
`// Copyright YYYY, VIXL authors`.

Gaby-VM rules (per `docs/README.md`):

- **Imported VIXL files keep the upstream header verbatim** — same
  year, same `VIXL authors`, same warranty text. Do not re-author.
- **New Gaby-VM files use the Gaby-VM 2026 BSD-3 header** documented
  in `docs/README.md`.
- Add an entry in `AUTHORS` referencing the upstream import scope at
  import time (e.g. "AArch64 simulator components imported from the
  VIXL project, BSD 3-Clause, https://github.com/Linaro/vixl").

## CMake implications

When the import lands, `src/CMakeLists.txt` will need to:

- Add a sources list grouped by tier (Tier 1 + 2 + 3 are compiled
  unconditionally; Tier 0 is *not* added).
- Define `VIXL_INCLUDE_TARGET_A64` (so `cpu-aarch64.h` compiles) and
  `VIXL_INCLUDE_SIMULATOR_AARCH64` (mirrors upstream simulator-mode).
- **Not** define `VIXL_CODE_BUFFER_*` — we do not import
  `code-buffer-vixl`, so the macro is irrelevant.
- **Not** define `VIXL_ENABLE_IMPLICIT_CHECKS` — the implicit-checks
  scaffolding is host-arch x86_64 only; leaving it undefined avoids
  the inline asm at `simulator-aarch64.cc:640-643`.
- Set the public include directory to point at `src/aarch64/` (so
  `#include "instructions-aarch64.h"` resolves) and add `src/` to
  the include path so the relative `#include "../globals-vixl.h"`
  resolves.

Sketch:

```cmake
set(GABY_VM_AARCH64_TIER1_SOURCES
  # shared root
  third_party/vixl/src/utils-vixl.cc
  third_party/vixl/src/compiler-intrinsics-vixl.cc
  third_party/vixl/src/cpu-features.cc
  # aarch64 simulator core
  third_party/vixl/src/aarch64/simulator-aarch64.cc
  third_party/vixl/src/aarch64/logic-aarch64.cc
  # decoder
  third_party/vixl/src/aarch64/decoder-aarch64.cc
  third_party/vixl/src/aarch64/instructions-aarch64.cc
  # auditor + cpu model
  third_party/vixl/src/aarch64/cpu-features-auditor-aarch64.cc
  third_party/vixl/src/aarch64/cpu-aarch64.cc
  # registers / operands / pointer auth
  third_party/vixl/src/aarch64/registers-aarch64.cc
  third_party/vixl/src/aarch64/operands-aarch64.cc
  third_party/vixl/src/aarch64/pointer-auth-aarch64.cc)

set(GABY_VM_AARCH64_TIER2_SOURCES
  third_party/vixl/src/aarch64/disasm-aarch64.cc)

set(GABY_VM_AARCH64_TIER3_SOURCES
  third_party/vixl/src/aarch64/debugger-aarch64.cc)

target_sources(gaby_vm PRIVATE
  ${GABY_VM_AARCH64_TIER1_SOURCES}
  ${GABY_VM_AARCH64_TIER2_SOURCES}
  ${GABY_VM_AARCH64_TIER3_SOURCES})

target_include_directories(gaby_vm PRIVATE
  third_party/vixl/src
  third_party/vixl/src/aarch64)

target_compile_definitions(gaby_vm PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64)
```

Path layout (`third_party/vixl/...` mirrors the upstream tree to keep
file headers' `#include "../globals-vixl.h"` working without
rewrites). Final layout decided when import lands.

## Things to flag during import

- **Inline x86_64 `__asm__`** at `simulator-aarch64.cc:640-643` is
  guarded by `VIXL_ENABLE_IMPLICIT_CHECKS`. Leave the macro undefined
  for iOS / non-x86_64 hosts.
- **`platform-vixl.h::HostBreakpoint()` raises `SIGINT`.** Acceptable
  on POSIX desktops; consider stubbing on iOS app builds where signal
  delivery is restricted.
- **`code-buffer-vixl.cc` uses `mmap` + `mprotect(PROT_EXEC)`.** Not
  imported, but worth recalling why we exclude codegen from V1.
- **`SimExclusiveLocalMonitor` / `SimExclusiveGlobalMonitor`**
  (`simulator-aarch64.h:1217-1281`) and the LSE atomic visitors in
  `simulator-aarch64.cc` are the *only* simulator code we expect to
  modify in-place after import — see
  [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
  on multi-instance + real atomic semantics. Mark them with a
  `// Gaby-VM deviation: ...` comment when the change lands; the rest
  of the imported files should stay bit-for-bit upstream so future
  merges from VIXL apply cleanly.
- **`Simulator::Simulator(...)`** (`simulator-aarch64.cc:645-706`)
  takes a self-allocated `SimStack::Allocated` by default. To accept
  an embedder-provided stack, two paths:
  - **(a) Extend `SimStack::Allocated`** to construct from an
    externally-owned `(base, size)` pair. Smallest in-place edit;
    keeps the Simulator constructor as-is.
  - **(b) Wrap the Simulator** with a Gaby-VM Simulator subclass that
    constructs `Memory` from a Gaby-VM-owned span. Zero diffs on
    imported VIXL files, more boilerplate.
  Pick during import; flagged here so the import author is aware.
- **`<mutex>` in `simulator-aarch64.h`** suggests at least one
  per-instance lock (likely in `MetaDataDepot`). Audit for
  thread-safety implications during the multi-instance work.

## Where to read next

- [`vixl-overview.md`](./vixl-overview.md) — high-level project map.
- [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md)
  — the imported components in detail.
- [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
  — Gaby-VM-specific changes after import.
