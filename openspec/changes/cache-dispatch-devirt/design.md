# Design: cache-dispatch-devirt

## Context

Authoritative architecture and rationale:
[`docs/refs/gaby-vm-fast-dispatch-synthesis-2026-07-03.md`](../../../docs/refs/gaby-vm-fast-dispatch-synthesis-2026-07-03.md)
(the C1 section and the H-findings). This design.md only pins C1-local
decisions; do not re-derive the architecture here.

Current dispatch chain (h:1725-1740 area, branch tip `bbc9aac`): load
`entry->leaf` → load 16-byte pmf → this-adjust + virtual-bit test → vptr →
vtable slot → call. All `Visit*` leaves are `virtual`; `Simulate_*` are not.

## Goals / Non-Goals

**Goals:** single-load single-indirect-call dispatch; handler ABI fixed for
C2-C7; byte-equivalent per-step semantics (the oracle must not be able to
tell C1 from its parent commit); applogic dispatch-hub share re-measured and
recorded.

**Non-Goals:** any continuation/threading semantics (handlers return
`nullptr`); any change to StepOnce/ShadowRunner behavior; any leaf or
epilogue behavior change beyond relocation into thunks.

## Decisions

**D1 — Handler shape.** `using GabyHandler = const
gaby_vm::PredecodeCache::PredecodedEntry* (*)(Simulator*, const
gaby_vm::PredecodeCache::PredecodedEntry*)`. Static member functions of
`vixl::aarch64::Simulator` inside a gaby marker block. The handler-contract
comment block (trivially-destructible locals only; RAII/hook work completes
before any future tail site; null-PC rule reserved for C2; epilogue
protocol) is written now so C3+ handlers inherit it.

**D2 — Thunk generation.** One macro stamped over the same visitor list that
builds `FormToVisitorFnMap` (`decoder-visitor-map-aarch64.h` — use the
`DEFAULT_FORM_TO_VISITOR_MAP`/`SIM_AUD_VISITOR_MAP` expansion lists). The
qualified call `sim->Simulator::VisitXxx(instr)` suppresses virtual dispatch
statically (a `template <auto pmf>` thunk would NOT devirtualize — known
trap). The thunk body must reproduce the exact per-step sequence currently
in `ExecuteInstructionCached` between entry-resolution and epilogue end.
Where the same form maps through `Simulate_*` (non-virtual shared leaves),
the same macro emits the qualified call to that member; `form_hash_` is
seated first in all cases (Simulate_* branch on it).

**D3 — Instruction pointer flow.** The thunk derives `instr` from
`sim->pc_` (not from the entry), preserving today's semantics exactly: the
hub currently passes `pc_` to the pmf. `pc_addr → entry` resolution stays in
`ExecuteInstructionCached`.

**D4 — Entry population.** `ResolvePredecodeHandler(form_hash)` returns the
handler fn-ptr (or nullptr → caller falls back to sentinel handler), stored
directly in `entry->leaf` via `const void*` (fn-ptr↔void* is fine on all
POSIX/Darwin targets; add a static_assert on pointer sizes). The two
sentinel leaves become sentinel *handlers* with identical abort messages.

**D5 — Byte-equivalence audit.** After the move, `ExecuteInstructionCached`
must contain exactly: pc→entry resolution (with `cur_range_` fast path,
GabyAbortPcNotInRange cold path) + one `AsGabyHandler(entry->leaf)(this,
entry)` call. Everything else lives in the thunk in the same relative order
as today. The T4 GABY_UNLIKELY hints move with their branches.

**D6 — Execution split.** Implementation by Opus-4.8 subagent(s); Fable
reviews the thunk-macro diff and the moved-sequence audit specifically.

## Risks / Trade-offs

- [Thunk sequence drifts from today's hub sequence] → the differential +
  absolute oracles across vixl_port catch semantic drift per form; a
  side-by-side code audit of old hub vs thunk body is a review gate.
- [A form resolves to a different member than the pmf map resolved] →
  vixl_port executes every reachable form on both tracks; mismatch = red.
- [I-cache growth from ~hundreds of stamped thunks] → accepted for C1
  (thunks are small; specialized handlers later replace the hot ones);
  measure text-size delta in numbers.md.
- [Bench moves less than predicted (~9.4-9.8 applogic)] → acceptance is
  ≥ neutral + ABI landed; the win prediction is secondary to fixing the ABI.

## Migration Plan

Single commit stack on `perf/cache-hotpath-tier1` (continuing the branch),
tasks in order, GRL per landing task; revert lever = flip the populate pass
back to pmf storage (one commit).

## Open Questions

(none — C2+ questions are deliberately out of scope)
