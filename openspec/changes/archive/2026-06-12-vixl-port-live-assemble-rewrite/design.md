## Context

`vixl_port` is the correctness guard rail for the predecode/dispatch optimization
([docs/testing.md](../../../docs/testing.md), [AGENTS.md](../../../AGENTS.md)). Today it is a
**frozen-fixture replay**: an authorship-time tool (`tools/vixl_test_extract/`) assembles each
upstream VIXL `TEST()` body to bytes, harvests expected state, commits the result, and the runner
replays the dead bytes on two gaby tracks. It works and is self-contained (no `../vixl` to build),
but cannot express memory access: upstream bodies bake a *host* address into the stream
(`Mov(reg, reinterpret_cast<uintptr_t>(buf))`), so the extractor's `IsNonPortableInstr` filter
drops every body with load/store/ADR/ADRP/register-indirect branch — 308 of 595 bodies skipped
(integer 174, fp 56, neon 78), and **zero LDR/STR/LDP/STP/atomic/exclusive/CAS coverage**. The
dispatch optimization runs straight through that access path.

Full aligned rationale (owner-signed 2026-06-09), file lists, and the marker text:
[the design doc](../../../docs/refs/gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md). This
document records the technical decisions; see the proposal for motivation and `specs/` for the
normative contract.

## Goals / Non-Goals

**Goals:**

- Assemble each upstream body **live at test time** against a real in-process scratch buffer, so
  baked addresses point at valid memory and load/store/ADR/literal run for real.
- Keep the two-track (cache vs decoder) + absolute/differential oracle model; close the
  memory-access blind spot so coverage approaches the full EL0 A64 set.
- Hold every existing hard boundary: no-JIT / no-RWX / iOS; assembler stays test-only and never
  links into `gaby_vm::gaby_vm`.

**Non-Goals:**

- No new IR, no JIT, no RWX, no native execution of assembled bytes.
- SVE is not run (no gaby SVE leaf); SVE assembler `.cc` files are copied only to satisfy
  out-of-line link symbols declared in the non-SVE headers.
- No change to imported simulator leaf semantics or to the shipping import (Tiers 1-3). Not a perf
  change.

## Decisions

**1. Replace the foundation; do not patch the frozen model.** Patching needs heavy machinery
(runtime buffer allocation, base-address seeding, baked-address erasure, a memory oracle, per-track
reset) and still only covers part. Live assembly makes the address problem vanish at the root:
`Mov(reg, reinterpret_cast<uintptr_t>(rig.scratch))` is encoded against a real in-process pointer,
and all three engines (VIXL reference sim, gaby cache track, gaby decoder track) share one address
space — VIXL's `Memory::Read/Write` is `reinterpret_cast<char*>(AddressUntag(addr))` + `memcpy`
(`simulator-aarch64.h:411-449`), so they see the same bytes.

**2. Copy the assembler into a test-only island; don't link `../vixl`.** Copying preserves the
existing "build and test without `../vixl`" property. The island lives at
`test/test_support/vixl_asm/` (under `test/`, naturally outside shipping compilation), carries a
plain-language test-only marker, and a `.clang-format` with `DisableFormat: true` to keep the
copies diffable against upstream. *Alternative — link `../vixl` directly:* rejected, it would make
`ctest` depend on the reference tree.

**3. Pin the import SHA = `160c445` (2026-05-14).** The copied `.cc` files are written against
upstream headers, but compile against gaby's hand-edited headers (the constexpr-inline
`VectorFormat` helpers, etc.). Same SHA → self-consistent. A mismatched SHA risks a **no-diagnostic
inline-body ODR**. The SHA is recorded in `vixl-extraction-map.md`; future VIXL upgrades re-sync
both halves together.

**4. ODR safety — the single hardest part.** MacroAssembler and Simulator share 8 leaf headers
already compiled into `gaby_vm` (instructions / operands / registers / constants / cpu-features /
utils / globals / compiler-intrinsics). The island therefore: **never re-copies** those 8 (copying
= same `vixl::` symbol defined twice → link clash); orders its include path with
`Sources/gaby_vm/src` **first**, so the copied assembler's `#include "operands-aarch64.h"` /
`"simulator-aarch64.h"` resolve to the already-imported copies (incl. the unconditional simulator
include at `macro-assembler-aarch64.h:41`, which lands on gaby's dual-track simulator — exactly what
we want); and **PRIVATE-links `gaby_vm::gaby_vm`** so shared leaf symbols come from `gaby_vm.a`,
defined once.

**5. no-RWX is load-bearing — refuse `test-utils.cc`.** VIXL's `test/test-utils.cc` has an
unconditional `#include <sys/mman.h>` + `ExecuteMemory()` that mmaps RWX and *natively calls* the
assembled bytes — a direct no-JIT/no-RWX/iOS violation. We do **not** copy it (`ExecuteMemory` is
unused on the simulator path — confirmed it sits in the `#else` branch of
`test-assembler-aarch64.h`). A gaby-authored `test-utils-stub.cc` supplies whatever symbol the test
infra actually links, with `ExecuteMemory` a no-op/abort-on-call. The code buffer is forced to
plain `malloc/free` via `VIXL_CODE_BUFFER_MALLOC` (PRIVATE to the island only); under that define
`SetExecutable/SetWritable` are `VIXL_UNIMPLEMENTED` no-ops — no `mprotect`, no executable page.

**6. Non-linkage into `gaby_vm` is enforced three ways.** Target named
`gaby_vm_vixl_asm_testonly` (no `::` alias, `_testonly` suffix → one grep reveals it can't ship),
added only under `GABY_VM_BUILD_TESTS`; `Sources/gaby_vm/src/CMakeLists.txt` stays byte-unchanged
(it already declares zero `target_link_libraries`, so nothing can pull the island back into
`gaby_vm.a`); test exes link it **PRIVATE**.

**7. Reuse the proven oracle + harness pieces.** The macro-redefinition trick
(`capture_macros.h`) that compiles upstream bodies verbatim stays; `RUN()` changes from "capture
frozen" to "run both gaby tracks now." Port `CheckAssert` / `DifferentialEqual` / `SeatEntry` from
`vixl_port_runner.cc` into `vixl_port_oracle.{h,cc}`, and the `sigsetjmp + alarm` crash/hang guard
from `extract_main.cc` into the new per-family `main()`. Entry state is explicitly seeded to a
state byte-equivalent to VIXL `Simulator::ResetState()` (`simulator-aarch64.cc:764`), with a
one-time equivalence assertion at startup.

## Risks / Trade-offs

- **SVE symbol completeness** → non-SVE headers declare out-of-line members defined in SVE `.cc`;
  mirror the extractor's `_vixl_srcs` source set; a link/`nm` smoke gate (migration step 3) exposes
  any gap immediately.
- **Silent inline-body ODR (header skew)** → pin the SHA (decision 3); record it in the extraction
  map; re-sync both halves on upgrade.
- **Stub symbol set drift** → the exact `test-utils-stub.cc` symbol set is discovered by the linker
  (expected: just `ExecuteMemory`); iterate to satisfy undefined symbols, never touch the mmap
  implementation.
- **Live fault / wild jump** → a body's `Mov(reg, host_addr)` + bad store can corrupt the test
  process; `BR/BLR` to a non-rig pointer lands on a wild PC. Mitigate: keep by-name isolation +
  crash/hang guard + per-case instruction cap; expose only the rig's registered scratch buffer.
- **Entry-state divergence** → if gaby's default-constructed state differs from VIXL `ResetState()`,
  the oracle could misjudge a read-but-never-written register. Mitigate: seed both tracks
  identically and assert equivalence once; seeding is authoritative, not the constructor default.
- **Coverage no longer frozen** → a live body newly hitting an unimplemented gaby leaf now *fails*
  instead of being pre-skipped. This is desirable (exposes real gaps), but changes the suite's
  character; new failures are triaged against the explicit deny-list / by-name surface.
- **Premature tool deletion** → the island + `gaby_vm.a` single-binary link has never been built.
  Mitigate: tool stays OFF-by-default until all three families are green; delete it last
  (reversible).

## Migration Plan

Nine ordered steps that keep `ctest -R vixl_port` green throughout, deleting the extraction tool
**last**; the canonical list (with exact file groups A/T/U/S and the marker text) lives in
[the design doc §6](../../../docs/refs/gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md). The
ODR/link `nm` smoke gate (step 3) is the hard gate before any harness work; integer family is
proven side-by-side under a `_live` name before the old fixtures are deleted, then fp and neon
repeat. `tasks.md` is the per-step checklist. Rollback at any step: the old frozen suite still
exists until its family is switched.

## Open Questions

- Exact `test-utils-stub.cc` symbol set — resolved empirically by the linker during step 4
  (expected to be `ExecuteMemory` alone).
- Whether any single family runs slowly enough to warrant further CTest subdivision — decided at
  step 5/6 once real wall-clock is observed.

## Implementation notes (benign deviations from this design)

The implementation reached the same goals via two mechanisms that differ from the wording above;
recorded here so the design text is not read as literal API:

- **No `rig.scratch` buffer; a frame-window reset instead.** Decision 1 sketches baking
  `reinterpret_cast<uintptr_t>(rig.scratch)`. The shipped harness has no dedicated scratch field:
  bodies bake the address of their *own C-stack locals*, and read-modify-write correctness across the
  three sequential engine runs is achieved by snapshotting and restoring the body's stack frame (the
  "frame window") between runs, which doubles as the memory oracle. See the frame-window section of
  [`docs/testing.md`](../../../docs/testing.md#ported-vixl-tests-vixl_port).
- **No `core.Dump`; the reference registers are read directly.** The absolute oracle does not emit or
  run VIXL's `core.Dump` code (that would run the reference at a different address than the gaby
  slice, diverging PC-relative results). Instead all three engines run the identical slice at the
  identical address and the reference sim's body-exit registers are read straight into a
  `RegisterFile`.

(The data-in-stream cache limitation surfaced by this suite — `ldr_literal*` / `fjcvtzs` skipping
because `RegisterCodeRange` rejects an unallocated data word — is tracked separately in
[`docs/refs/gaby-vm-predecode-cache-data-in-stream-2026-06-10.md`](../../../docs/refs/gaby-vm-predecode-cache-data-in-stream-2026-06-10.md).)
