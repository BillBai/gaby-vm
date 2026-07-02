# Design: cache-hotpath-tier1

## Context

Current cache-track cost picture (sources: 2026-06-11 dispatch-flatten
profile, 2026-07-02 exploration workflow `wf_f8cc59b0-fa6` + adversarial
review, fresh 2026-07-03 `applogic` sample profile at
`-O3 -g -fno-omit-frame-pointer`):

- Dispatch hub (`ExecuteInstructionCached`) is 38–42% of self-time on scalar
  business kernels; mostly fixed bookkeeping, not the indirect call.
- MOVPRFX per-step classification measured at ~6% via ablation (form_hash_
  read/write dependency chain + a bool live across the leaf call).
- `applogic` (FP shape): `memset`/`memmove`/`bzero`/`chkstk` ≈ 15% of samples
  — `LogicVRegister`'s SVE-max scratch arrays (`saturated_[1024]`,
  `round_[256]`, `simulator-aarch64.h:1086-1089`) are zeroed per construction
  (h:781-790) and copied by value through the FP helper chain, while VL is
  pinned to 128 (`simulator-aarch64.cc:684`).
- Load/store helpers run a dead trace-preparation tail unconditionally with
  tracing off (cc:4677-4699 and the pair variant), plus `LocalMonitor::
  MaybeClear`'s LCG on every access (h:1273-1280).
- Every non-ret BR/BLR probes a `std::unordered_map` for branch interceptions
  (cc:4072-4086) that embedders never register on the cache track.
- `AddWithCarry` computes NZCV unconditionally and discards it when
  `set_flags` is false (cc:1049-1100).

Working perf baseline for this change (M1 Pro, dev-release, 2026-07-03):
parse 9.34 / hash 10.49 / struct 10.81 / fsm 9.34 / applogic 14.26 ns/insn.
Doc baselines (M4 Pro: ~6.5 scalar / ~10.0 applogic) are **not** comparable
to numbers measured on this machine.

## Goals / Non-Goals

**Goals:**

- Land the six Tier-1 items (T1–T6, see proposal) with per-item before/after
  `bench_business` numbers on the same host.
- Keep `vixl_port` green and `bench_business --verify` bit-identical at every
  step; each item is one commit (T2's LCG edit gets its own commit).
- Close the MOVPRFX test-coverage hole before changing its dispatch-side
  implementation.

**Non-Goals:**

- No entry-layout, `form_hash_`, public-API, or harness changes.
- No flatten/specialize/threaded-dispatch work (Tier 2/3 follow-ups).
- No attempt to reproduce the M4 Pro doc baselines on this host.

## Decisions

**D1 — T1 formulation: shrink the object, don't change helper signatures.**
The exploration's alternative (by-ref `dst` returns in the
`DEFINE_NEON_FP_VECTOR_OP` family) was rejected by review: call sites pass
temporaries that cannot bind to non-const refs, making it a call-site-wide
rewrite of imported interfaces. Shrinking `LogicVRegister`'s arrays to
`kGabyMaxLanes = 16` (max lanes addressable at VL=128: 16 × B lanes per
128-bit vector) collapses the same copies without touching any signature.
Array *extent* change on an imported member is read as a permitted
structural modification under the aarch64-simulator spec's marker
convention (members are neither removed nor renamed); recorded here as the
on-record interpretation the review asked for.

**D2 — T1 safety net: VL guard, not test faith.** `vixl_port` has **no SVE
execution bodies** (island contains assembler/fp/neon suites only), and the
island's `TEST_SVE` machinery would run vl512/vl2048 variants if SVE coverage
is ever widened. So T1 ships with: a `static_assert` tying the bound to
`kZRegMinSizeInBytes` (VL=128), a runtime `VIXL_ASSERT` in
`SetVectorLengthInBits` rejecting VL > 128 while the bound is in force, and a
note in `docs/refs/vixl-extraction-map.md` that widening SVE coverage must
revisit the bound.

**D3 — T3 mechanism: previous-entry bit, form_hash_ untouched.** Predecode
sets `flags` bit 1 = "this form is MOVPRFX" (bit 0 is the existing
BTI-relevant bit). `ExecuteInstructionCached` keeps a member bool
`prev_was_movprfx_` written post-leaf from the already-loaded entry flags and
read pre-leaf, eliminating the two `form_hash_` compares and the cross-call
live bool. The `form_hash_` store stays (shared `Simulate_*` leaves and the
re-entrancy cursor depend on it), so the frozen-layout and cursor spec
scenarios are untouched. `prev_was_movprfx_` joins the re-entrancy cursor
save/restore set (a nested RunFrom must not corrupt the enclosing run's
MOVPRFX chain, mirroring `form_hash_`/`last_instr_` handling).

**D4 — T3 tests first.** Hand-encoded MOVPRFX unit tests land *before* the
dispatch change: positive (MOVPRFX + legal consumer executes; cache ==
decoder) and negative (MOVPRFX + illegal consumer aborts via
`CanTakeSVEMovprfx` on both tracks). Encodings are hand-assembled words (the
test island's assembler has no SVE), executed at VL=128.

**D5 — T2 LCG deviation is isolated and documented.** Early-returning
`MaybeClear` when the monitor is unarmed changes the LCG seed *sequence*
relative to upstream. All three in-repo simulators share the class, so the
differential and absolute oracles stay aligned; but a future island re-sync
could import an upstream test that asserts a specific STXR status without a
retry loop. Own commit + marker comment naming exactly this hazard.

**D6 — T6 is disassembly-gated.** Both `AddWithCarry` overloads live in one
TU; clang at `-O3` may already sink the flag computation into the
`set_flags` arm. First disassemble `AddSubHelper`'s call path in the release
build; if the NZCV chain is already sunk, T6 is recorded as no-op and
skipped. No speculative patch.

**D7 — Execution split.** Implementation tasks are delegated to Opus-4.8
subagents (one item at a time, each with the full guard-rail loop);
Fable reviews diffs, adjudicates surprises, and owns the final numbers table.

## Risks / Trade-offs

- [T1 lane bound trips future SVE work] → D2's triple guard (static assert,
  runtime assert, extraction-map note); revisit when SVE coverage widens.
- [T3 flag misclassification silently skips the MOVPRFX check] → D4's
  negative test fails if the check stops firing; predecode classification is
  two exact form-hash compares at registration time.
- [T2 trace-tail gate accidentally changes decoder-track trace output] → the
  gate tests the same trace mask the logging functions test; decoder-track
  trace tests in `vixl_port` (trace-enabled paths) stay green.
- [Estimates don't add up across items] → per-item before/after measurement;
  acceptance is "each item ≥ neutral, tier total meaningfully positive",
  not the paper sum (~10–15% scalar, ~25–30% applogic).
- [Benchmark noise on a shared dev machine] → 3 runs per side, quote ranges,
  machine otherwise idle (no concurrent agents during timing).

## Migration Plan

Six commits on `perf/cache-hotpath-tier1` (T2 = two commits), each:
build → `ctest -R vixl_port` (debug + release presets) → `bench_business
--verify` → 3× `bench_business` per shape → record numbers in the tasks
checklist. Branch stays unmerged (explicit instruction); final state is a
clean stack ready for review plus a numbers table in the change directory.
Rollback = revert the offending commit; items are independent.

## Open Questions

- (resolved-by-measurement) T6 go/no-go after the disassembly check.
- Whether the T4 `ostringstream` extraction keeps the exact abort message
  text (it should; message content is asserted nowhere but useful).
