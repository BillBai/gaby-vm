# Design: cache-threaded-runfrom

## Context

Architecture authority:
[`docs/refs/gaby-vm-fast-dispatch-synthesis-2026-07-03.md`](../../../docs/refs/gaby-vm-fast-dispatch-synthesis-2026-07-03.md)
— C2 section, findings H1/H2/H3/H7, musttail portability section (§4).
C1 (`efce14c`) landed the handler ABI: ~460 statically-bound thunks with a
shared force-inlined prologue/epilogue, `return nullptr` reserved for this
change. Current numbers (C1 row): parse 8.116 / hash 6.654 / struct 8.648 /
fsm 8.057 / applogic 9.918.

## Goals / Non-Goals

**Goals:** no central loop on the threaded path; per-handler tail-dispatch
sites; range check off the straight-line path; H1/H2/H3 protocols exactly as
synthesized; equivalence provable by tests (threaded == stepped == decoder).

**Non-Goals:** handler-body specialization; any change to what a step
observes (per-instruction state remains bit-identical); `preserve_none`.

## Decisions

**D1 — Dispatch macro (H1 verbatim from the synthesis).**
```cpp
#define GABY_DISPATCH(sim, next)                                    \
  do {                                                              \
    if (GABY_UNLIKELY((sim)->gaby_step_mode_)) return (next);       \
    GABY_MUSTTAIL return AsGabyHandler((next)->leaf)((sim), (next)); \
  } while (0)
```
`musttail` applies per return statement; the guarded early return is legal.
`GABY_MUSTTAIL` = `[[clang::musttail]]` when available and `GABY_THREADED`,
else empty (loop fallback: caller loops `while (e != nullptr) e = h(e)`).
Terminal handler returns `nullptr` in both modes (loop exit / chain end).

**D2 — Step mode is harness state, not cursor state.** `gaby_step_mode_`
member beside `trace_parameters_` handling: `ExecutionScope` (gaby_vm/
simulator.cc) saves/sets/restores it — RunFrom sets false, StepOnce sets
true, nesting restores the enclosing value. It must NOT join
`GabyInterpreterCursor` (it describes the harness, not the interpreter
position).

**D3 — Continuation protocol (generic thunk epilogue).** After the existing
epilogue: if `pc_modified_` is false → `GABY_DISPATCH(sim, entry + 1)`
(boundary sentinel guarantees `entry + 1` is always valid); else null-check
`pc_` (→ terminal, H2), then re-derive the entry via the `cur_range_` fast
path + `FindRange` (miss → `GabyAbortPcNotInRange`, unchanged) and
`GABY_DISPATCH` it. After ANY re-entrant call (branch hook), the
continuation is re-derived from members — never from stale locals (H7).

**D4 — Boundary sentinel (H3).** Populate allocates `size/4 + 1` entries;
the last gets a boundary handler: re-resolve `pc_` via `FindRange`
(adjacent range → continue chaining there), null → terminal, none → abort.
Public `CodeRange` comment updated; `(pc-start)/4` indexing unchanged for
real words.

**D5 — RunFrom / StepOnce split.** Threaded `RunFrom`: seat PC, resolve
first entry (existing resolution incl. abort path), call the handler once —
the chain runs to termination (or, fallback mode, loop on returns).
`StepOnce` keeps calling `ExecuteInstructionCached` (lookup + one call);
with step mode set the handler returns its continuation, which
`ExecuteInstructionCached` discards — per-step behavior identical to C1.
The chain runs with ordinary C stack frames; a hook calling nested
`RunFrom` mid-chain is ordinary re-entrancy (H7): scope saves cursor +
step-mode, nested chain runs, returns, enclosing handler re-derives from
`pc_`.

**D6 — Handler-frame hygiene.** Musttail requires no live non-trivial
destructors across the tail site: handler locals stay trivially
destructible (already true in C1 thunks; the contract comment gets the
rule). Exceptions stay enabled globally; cold aborts stay outlined.

**D7 — CI matrix (synthesis §4).** Two additions, wired via `ci/` per
`docs/refs/ci.md` conventions: (a) a debug/-O0 build+ctest job with
threading ON (musttail is guaranteed at -O0 — this catches
accidentally-non-tail code); (b) a `GABY_THREADED=0` build+ctest job
(fallback loop). Both run vixl_port + the new equivalence tests.

**D8 — Execution split.** Opus implements; Fable reviews the dispatch
macro, continuation protocol, and the new tests specifically. Numbers
gate: expect meaningful improvement (prediction band ~8.4-9.2 applogic
from 9.918); accept ≥ neutral; STOP on >2% consistent regression.

## Risks / Trade-offs

- [musttail silently absent → stack growth] → the -O0 threaded CI job +
  a deep-chain smoke test (a range with ~1M straight-line instructions
  executes without stack exhaustion) make it structural, not hopeful.
- [step-mode gate desync under nesting] → ExecutionScope owns it (D2);
  the hook-in-chain test nests RunFrom inside StepOnce and vice versa.
- [boundary sentinel breaks `--verify` dynamic counts] → sentinel is never
  executed on well-formed code; data-in-stream tests + vixl_port cover.
- [fallback loop bit-rots] → CI job (D7b) keeps it green permanently.

## Migration Plan

Two commits: (1) mechanics + tests, (2) CI wiring. GRL each. Revert lever:
`GABY_THREADED=0` compile-time kill switch, then commit revert if needed.

## Open Questions

(none)
