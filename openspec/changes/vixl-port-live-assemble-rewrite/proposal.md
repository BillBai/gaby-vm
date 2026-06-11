## Why

The `vixl_port` suite is the guard rail for the predecode/dispatch optimization, but its
"frozen fixture replay" model has a structural blind spot: **it cannot express memory-access
semantics.** Upstream VIXL test bodies bake a host address into the instruction stream
(`Mov(reg, reinterpret_cast<uintptr_t>(buf))`), so the extraction tool's `IsNonPortableInstr`
filter drops every body containing a load/store/ADR/ADRP/register-indirect branch — ~174 of
264 integer bodies, and zero LDR/STR/LDP/STP/atomic/exclusive/CAS coverage. The dispatch
optimization runs straight through the access path the guard rail can't see. Patching the
frozen model needs heavy machinery (runtime buffer seeding, baked-address erasure, a memory
oracle); replacing the foundation is cleaner. Full rationale and aligned design:
[design doc](../../../docs/refs/gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md).

## What Changes

- Link VIXL's assembler into a **test-only island** at `test/test_support/vixl_asm/` (a copy of
  Tier-0 assembler files at pinned SHA `160c445`), so each upstream `TEST()` body is **assembled
  live at test time** against a real in-process scratch buffer. Baked-address breakage disappears;
  load/store/ADR/literal run for real, and coverage reaches close to the EL0 A64 set.
- Run every assembled body on **both gaby tracks** (cache + decoder) under an **absolute oracle**
  (VIXL reference sim) and a **differential oracle** (cache track == decoder track), each over
  **registers and memory**. Store *results* become real checks — not via the upstream
  `ASSERT_EQUAL_MEMORY` macro (unused in the ported non-SVE bodies, so it stays a skip mark) but via
  a frame-window memory oracle that compares each body's exit store image across all three engines.
- Define `VIXL_CODE_BUFFER_MALLOC` for the island only: assembled bytes are ordinary `malloc`
  data fed to the decoder, **never natively executed**. no-JIT/no-RWX/iOS unbroken.
- Drop the structural `IsNonPortableInstr` filter; keep the capability-based skip surface (feature
  deny-list, by-name isolation) and the crash/hang + per-case instruction-cap guards.
- **BREAKING (test infra only):** remove the frozen-fixture model — `vixl_port_fixture.h`,
  `vixl_port_runner.{h,cc}`, `generated/`, the manifest — and, last, the `tools/vixl_test_extract/`
  extraction tool plus its `GABY_VM_BUILD_VIXL_EXTRACT` option.

## Capabilities

### New Capabilities

(none — the suite is the correctness guard rail *of* the imported simulator, not a separate
product capability, and the requirements it touches already live in `aarch64-simulator`)

### Modified Capabilities

- `aarch64-simulator`: scope the Tier-0 import ban to the *shipping* tree (`Sources/gaby_vm/src/`)
  and permit a non-linkable test-only Tier-0 copy under `test/`; scope the `VIXL_CODE_BUFFER_MALLOC`
  / `VIXL_CODE_BUFFER_MMAP` ban to `gaby_vm` sources and require `MALLOC` (still forbid `MMAP`) for
  the island; add requirements for the live-assemble two-track suite, its memory-access coverage,
  and the island's no-RWX / non-linkage guarantees.

## Impact

- New: `test/test_support/vixl_asm/` island (copied Tier-0 assembler + VIXL test infra + a
  gaby-authored `test-utils-stub.cc`), new two-track macros + oracle, test target
  `gaby_vm_vixl_asm_testonly` (PRIVATE-linked, no `::` alias, behind `GABY_VM_BUILD_TESTS`).
- Removed: frozen fixtures, runner, manifest, and (last) the extraction tool.
- Docs: `docs/refs/vixl-extraction-map.md` (Tier-0 now a test-only copy + pinned SHA),
  `docs/architecture.md` (import boundary: shipping Tiers 1-3 vs test-only Tier-0),
  `docs/testing.md` (live-assemble two-track, no frozen fixtures), `AGENTS.md` (drop the
  regenerate-fixtures sentence).
- `Sources/gaby_vm/src/CMakeLists.txt` stays byte-unchanged; the island cannot leak into
  `gaby_vm.a` (zero link-deps + PRIVATE link + no `::` alias).

## Non-goals

- No new IR, no JIT, no RWX, no native execution of assembled bytes.
- SVE is not run (no gaby SVE leaf); the SVE assembler `.cc` is copied only to satisfy out-of-line
  link symbols declared in the non-SVE headers.
- No change to imported simulator leaf semantics or to the shipping import (Tiers 1-3).
- Not a performance change; the `bench_*` harness is untouched.
