# Testing strategy

This is the orientation doc. Detailed, normative requirements for each test
target live in
[`../openspec/specs/aarch64-simulator/spec.md`](../openspec/specs/aarch64-simulator/spec.md);
this document explains the shape so you know *which* spec section to consult.

## Current shape

Tests are stand-alone executables under [`../test/`](../test/), each built
from a single project-authored translation unit, linked against
`gaby_vm::gaby_vm`, and registered with CTest via `add_test`.

The project does not currently use an external testing framework like
GoogleTest or Catch2 — mostly because nobody has needed to pull one in yet.
Hand-rolling the few existing tests has been simpler than integrating a
framework. This is not a long-term position; if the test set grows or starts
needing things like parameterised fixtures, picking up a framework is fine.

When a test needs to reach `vixl::aarch64` types directly (e.g. construct a
`Simulator`), it uses the
[privileged build pattern](build.md#privileged-test-build-pattern):
`PRIVATE` include of `${PROJECT_SOURCE_DIR}/src` plus the same `VIXL_*`
compile definitions that gate the imported sources. Tests that only exercise
the public API don't need that privilege.

## Current targets

- `gaby_vm_smoke` — public-API sanity check. Doesn't reach into imported
  headers.
- `simulator_smoke` — constructs a `vixl::aarch64::Simulator`, decodes and
  executes a single NOP (`0xd503201f`) through the imported
  `Decoder → VisitNamedInstruction → leaf` flow, exits 0.
- `simulator_correctness` — baseline correctness regression. Drives
  hand-encoded `uint32_t` instruction sequences through
  `Simulator::RunFrom` and checks post-run register / memory state. Today it
  covers integer arithmetic (`ADD`, `SUB`, `MUL`), logical (`AND`, `ORR`,
  `EOR`), load/store with both immediate-offset and register-offset modes,
  conditional control flow (`B.cond` after `SUBS`/`CMP`, `CBZ`, `CBNZ`), and
  procedure call / return (`BL` + `RET`). Each sequence terminates via a
  trailing `RET` to a NULL link register, relying on the
  `LR == kEndOfSimAddress` invariant established by
  `Simulator::ResetRegisters`.
- `vixl_port_integer` / `vixl_port_fp` / `vixl_port_neon` — the broad
  correctness guard rail, machine-ported from VIXL's own
  `test-assembler-{aarch64,fp-aarch64,neon-aarch64}.cc`. Far wider coverage
  than `simulator_correctness` (hundreds of instruction forms). See
  [Ported VIXL tests](#ported-vixl-tests-vixl_port) below.

## Ported VIXL tests (`vixl_port`)

`vixl_port` is a large correctness guard rail ported from VIXL's own execution
test suite, built specifically to catch regressions in the shared execution hot
path before the dispatch / operand-predecode performance work. Aligned design:
[`refs/gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md`](refs/gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md)
(it supersedes the earlier frozen-fixture design, `-vixl-sim-test-port-*`).

It **live-assembles** each upstream `TEST()` body at test time and runs it on
both gaby tracks. There are no committed fixtures and no extraction tool — the
suite assembles fresh on every run.

### How it works

- **Test-only assembler island** — `test/test_support/vixl_asm/`. A copy of the
  Tier-0 VIXL AArch64 assembler + macro-assembler + code-buffer and the VIXL
  test infrastructure, taken verbatim from `../vixl` at the import SHA pinned in
  [`refs/vixl-extraction-map.md`](refs/vixl-extraction-map.md). It is compiled
  into a `gaby_vm_vixl_asm_testonly` static library that PRIVATE-links
  `gaby_vm::gaby_vm` (so the shared leaf/simulator symbols come from `gaby_vm.a`,
  defined once — no ODR clash), and it is **never** linked into the shipping
  library: no `::` alias, a `_testonly` suffix, and gated behind
  `GABY_VM_BUILD_TESTS`. The island builds `VIXL_CODE_BUFFER_MALLOC` only — the
  assembled bytes are ordinary `malloc` data fed to the gaby decoder, never
  executed on the host CPU (no-JIT / no-RWX / iOS holds). See the VIXL import
  boundary in [`architecture.md`](architecture.md#vixl-import-boundary).
- **Two-track harness** — `test/test_support/vixl_asm/harness/`. The
  `gaby_two_track_macros.h` header redefines VIXL's `SETUP/START/END/RUN/
  ASSERT_EQUAL_*` so an upstream test `.cc` compiles verbatim (its no-guard
  self-include is stripped at copy time). `RUN()` assembles the body into a
  `malloc` buffer, then runs the SAME bytes at the SAME address on three engines:
  a VIXL reference `Simulator` (the absolute-oracle anchor) and gaby's two tracks
  (cache via `RunFrom`, decoder via `DebugRunFrom`). The three CTest mains
  (`vixl_port_integer` / `_fp` / `_neon`) each include one upstream `.cc` plus the
  per-family `main` (the Test-list walk + a `sigsetjmp`/`alarm` crash-hang guard).

Two oracles gate every included body, each over **registers and memory**:

- **Differential** — the two gaby tracks must match: the full `RegisterFile`, and
  the body's exit memory image (the store window — see below). This is the primary
  catch for a cache-track dispatch regression.
- **Absolute** — gaby must match the reference simulator's body-exit values (read
  directly; no `core.Dump`). For registers, every `ASSERT_EQUAL_*` register/flag is
  pinned to the reference — and where an assert relates **two** registers
  (`ASSERT_EQUAL_64(x4, x5)`, `ASSERT_NOT_EQUAL_64(x1, x3)`) **both** are pinned, not
  just the result, so a track that mis-computes the *other* operand (breaking the
  asserted equality / inequality while the result stays correct) is caught. Each
  register operand is read from its **own bank**: `ASSERT_EQUAL_64(<bits>, d17)` is a
  V-register compare (the low 64-bit lane of `v17`), not the same-numbered `x17` — the
  classifier keys on `IsVRegister()` rather than just "has a register code", so an X
  and a V register sharing a code are never confused. `ASSERT_EQUAL_REGISTERS` pins the
  **whole** register file (the reference ran the identical bytes from the identical
  entry, so its exit state is the ground truth for "nothing was clobbered"). For
  memory, the body's exit store image must match
  the reference's. All three engines run the identical slice at the identical
  address, so PC-relative results (ADR/literal) agree. (`ASSERT_LITERAL_POOL_SIZE`
  is a deliberate no-op: it inspects the test-only island assembler's pool, not a
  gaby leaf property, so it is out of the execution oracle's scope.)

Because the body is assembled live, `Mov(reg, reinterpret_cast<uintptr_t>(buf))`
bakes a real in-process address, so **load/store/LDP/STP/ADR/literal AND
read-modify-write (atomic / exclusive / CAS, NEON store-multiple) bodies run for
real** against valid memory — coverage the previous frozen-fixture model
structurally could not express. Crucially the *results* of those stores are
checked, not just executed: the memory oracle compares the store window across all
three engines, so a store the shared leaf gets wrong on both tracks (which the
register snapshots cannot see) is caught against the reference.

RMW bodies need one extra thing: the three engines run the SAME body against the
SAME baked-address locals, sequentially, so an earlier engine's store would
otherwise leak into a later engine's load. The harness resets that memory between
runs. The body's locals live in the body function's stack frame, so `RUN()`
captures the body's frame pointer (`__builtin_frame_address(0)`, which the macro
inlines into the body) and `TwoTrackRun` snapshots the window `[its own frame,
the body frame)` once, then restores it before each engine run. That window is
exactly the body's frame — the baked-address locals — and sits entirely above the
harness's own active frame, so the restore resets the body's memory without
touching harness state. (The per-case `LiveRig` is a stack local in that frame,
but the register-file snapshots the oracle fills *during* the runs are held in
process-lifetime storage, outside the window.) The guest stack (`gaby_stack`, a
separate buffer) is zeroed alongside.

That same window is what the **memory oracle** reads: after each engine finishes
(and before the next reset) the harness captures the window bytes, then compares
the three captures. Only the guest's stores to the body's locals differ between
captures (the rest of the frame — `rig_`, padding — is byte-identical across runs),
so a mismatch is a genuine store-result divergence. When the frame is too large to
window safely (>256KB) or its frame pointers read back implausibly, the window is
**inactive** and the memory oracle and the reset are both skipped for that body;
the harness prints a one-time `[warn]` line naming the first such body so this
degradation is visible rather than silent. The smoke family includes a dedicated
read-modify-write body (`harness_smoke_memory`) whose correctness *depends* on the
reset+capture working, so the window mechanism is exercised on every run rather
than only by the large upstream RMW bodies.

Caveat: the window is a raw `memcpy` of a live C++ stack frame, so it is
**incompatible with AddressSanitizer** — ASan's stack red-zones would be copied
and trip a false report. Do not build the `vixl_port` families under ASan; the
two-track + reference differential is the memory-safety check here, not a
sanitizer. (See the I8/MemoryWriteSink note in the review docs for a record/undo
alternative that would remove this constraint.)

### What gets skipped, and why

Inclusion is no longer gated by a structural load/store / PC-relative filter (the
former `IsNonPortableInstr` is gone). The remaining skip surface is capability-
or determinism-based, and the per-family run prints a count (set `VIXL_PORT_TRACE=1`
for per-case reasons):

- **feature deny-list** — a body whose *seen* features include MTE / PAuth / GCS /
  HBC / BF16 / TME / WFXT (gaby cannot execute them, or the result is key /
  modifier / host-address dependent).
- **by-name quarantine** — (a) simulator-runtime / system bodies (printf, runtime
  calls, branch interception, MOPS, `dc_zva`, `system`, `large_sim_stack`,
  `generic_operand`); (b) `configure_cpu_features*`
  (tests the feature-config mechanism on the reused reference sim), `branch_to_reg`
  (register-indirect branch into BTI-guarded code), and `gcs_feature_off` (disables
  the Guarded-Control-Stack check on the *reference* sim only — the gaby public API
  exposes no GCS-disable, so the two tracks would abort on its deliberately-mismatched
  return; GCS is out of V1 scope). The read-modify-write families (atomics /
  exclusives / CAS, NEON store-multiple) are **not** quarantined — the per-run memory
  reset above lets them run on both tracks.
- **crash/hang during assembly or the reference run** — a body that faults, hits a
  fatal (debug-only) check, or runs past the per-case instruction cap (a loop on an
  uninitialised count register, or far-branch padding) while *assembling* or under the
  *reference* sim is reported as skipped: it is an unportable body or a leaf the
  reference itself rejects, not a gaby divergence. A crash/hang inside a **gaby track**
  is the opposite — see the crash guard below.
- **cache cannot register the slice** — a body whose inline literal pool contains
  a data word that decodes to an *unallocated* encoding when the cache predecodes
  the whole range (data-in-stream); `RegisterCodeRange` rejects the range, so the
  body is reported as a skip. This is a real product limitation, not a gaby
  divergence — tracked for a fix in
  [`refs/gaby-vm-predecode-cache-data-in-stream-2026-06-10.md`](refs/gaby-vm-predecode-cache-data-in-stream-2026-06-10.md)
  (predecode the data word into a non-executable sentinel entry instead of
  rejecting). Today it affects `ldr_literal*` and `fjcvtzs`.
- **legitimate disagreements** — e.g. an ADRP body whose baked expected literal
  is address-dependent, or a body asserting an absolute `sp` value; the reference
  computes correctly but the upstream literal does not match under the harness's
  address/sp, so the case is skipped rather than failed.

SVE is not run (no gaby SVE leaf); the SVE assembler `.cc` is copied only to
satisfy out-of-line link symbols the non-SVE headers declare.

### Determinism caveats (where three-engine agreement rests on shared state)

Two bodies pass only because a *random-looking* value comes out identical on all
three engines, and that identity is not separately enforced — worth knowing
before trusting these cases:

- **`system_rng`** (RNDR / RNDRRS): the three engines agree because they share a
  deterministic, identically-seeded PRNG sequence, so `x1`/`x3`/`x5`/`x7` match.
  The agreement is real but incidental to seeding, not to instruction semantics.
- **Exclusive-monitor PRNG**: the imported simulator's exclusive-monitor backoff
  uses a PRNG seed that `ResetState()` does **not** reset, so the reference and
  gaby tracks could in principle drift after the first exclusive sequence. In
  practice the upstream `ldxr`/`stxr` bodies use the canonical retry-loop shape,
  which converges regardless of the monitor's internal coin-flips, so the
  families stay green — "lucky soundness" that holds today but is not guaranteed
  by a reset. If an exclusives body is ever added that depends on a specific
  monitor outcome, seed parity across the engines would need to be made explicit.

### Gaby-track crash guard (a crash here FAILs, it does not skip)

The per-family main runs each body under a `sigsetjmp`/`alarm` guard and tracks
*which engine* is executing (`g_run_phase`). A fatal signal (SIGSEGV / SIGBUS /
SIGILL / SIGABRT / SIGALRM) that fires while a **gaby track** (cache `RunFrom` or
decoder `DebugRunFrom`) is running is the exact dispatch / predecode regression
class this guard rail exists to catch — a wild PC, an out-of-range cache entry, a
runaway loop — so it is recorded as a **FAILURE**, and the family stops there. It
stops rather than continuing because the signal unwound past gaby's
`ExecutionScope` destructor, leaving the shared imported `Simulator`'s `busy`
re-entrancy flag latched true; every later run on that engine would be silently
misinterpreted as nested. The FAIL already makes the suite red — fix and re-run.
(A signal during assembly or the reference run stays a skip, as above.)

Because no body in the live suite currently crashes a gaby track, this path has
no natural trigger — so `vixl_asm_harness_selftest` injects a fatal signal at a
chosen phase (a `GABY_VM_VIXL_PORT_SELFTEST`-gated seam, a no-op in the family
binaries) and asserts the harness FAILs + halts for a cache- or decoder-phase
fault, and *skips* (does not halt) for a reference-phase fault. It runs on every
`ctest`, so the logic is exercised continuously rather than first on the day it
matters.

### Coverage baseline (green must mean "still covering what it covered")

The suite would otherwise return green on any `failed == 0` run, so a regression
that silently moves cases from "ran" to "skipped" (a leaked sim knob like the
guarded-pages bug below, a widened feature deny, a new gaby-track abort) would
shrink coverage invisibly. The per-family main therefore pins the expected
ran/skipped split (`kFamilyBaselines`) and FAILs on any drift. The baseline
carries a pair per config (selected by `NDEBUG`) for the case a debug-only host
assertion shifts a body from "ran" to "skipped"; today debug and release agree
for every family. (They differed historically by one integer case,
`branch_tagged_and_adr_adrp`: the assembler's debug-only
`AllowPageOffsetDependentCode()` assertion aborted its `adrp`-to-label assembly
until `SETUP_CUSTOM` was fixed to honour the upstream PIC request, so the body
now assembles and runs under both configs — see `gaby_two_track_macros.h`.) To
re-baseline after an intended change, run with
`VIXL_PORT_REBASELINE=1` (prints the observed split instead of failing) and update
the table in the same commit. The drift decision is a pure function
(`IsBaselineViolation`) with its own check in `vixl_asm_harness_selftest`, so both
directions — match passes, drift fails, rebaseline suppresses — are regression-
tested, not just the "counts matched" half a green run demonstrates.

### Refreshing after a VIXL upgrade

There are no fixtures to regenerate. To re-sync the island to a new upstream:
`git -C ../vixl checkout <new-sha>`, re-copy the island files at that SHA,
update the pinned SHA in [`refs/vixl-extraction-map.md`](refs/vixl-extraction-map.md)
(the imported simulator headers and the island assembler must come from the same
SHA — a skew risks a no-diagnostic inline-body ODR), and re-run `ctest -R vixl_port`.

## Encoding policy

The shipping tree (`Sources/gaby_vm/src/`) ships no assembler or instruction-emit
helper — Tier 0 of the VIXL import is excluded from `gaby_vm` (the one Tier-0
copy is the test-only `vixl_port` assembler island above, never linked into
`gaby_vm`). So `simulator_correctness` does not assemble at runtime; its
instruction sequences are raw `uint32_t` arrays.
When a new encoding is needed at authorship time, an external tool such as
`llvm-mc` is a reasonable way to produce the hex, which then gets hand-copied
into the test source. This keeps the build and runtime free of the assembler
machinery without preventing us from leaning on external tools while
authoring tests.

## What's expected from a new test

- Add an `add_executable` + `add_test` pair to
  [`../test/CMakeLists.txt`](../test/CMakeLists.txt).
- Apply `gaby_vm_apply_compile_flags` (the project-policy helper). The
  imported-source relaxation helper is generally not appropriate for tests.
- If the test needs imported headers, use the privileged build pattern.
- The test should pass under the `dev-debug` preset; release builds are
  exercised via `ctest --test-dir build/release` until a release test preset
  is added.
- On failure, exit non-zero with stdout that names the failing sub-case and
  shows actual vs. expected values.
