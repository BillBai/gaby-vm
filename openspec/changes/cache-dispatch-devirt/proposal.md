# Proposal: cache-dispatch-devirt

## Why

C1 of the fast-dispatch plan
([`docs/refs/gaby-vm-fast-dispatch-synthesis-2026-07-03.md`](../../../docs/refs/gaby-vm-fast-dispatch-synthesis-2026-07-03.md)):
the cache track dispatches through a triple indirection — `entry->leaf`
points at pmf storage, the 16-byte Itanium pmf is loaded, and because every
`Visit*` is `virtual`, the call still walks vptr + vtable — feeding the
loop's only unpredictable indirect branch. Replacing it with macro-generated,
statically-bound flat handler functions removes the dependent-load tail,
fixes the handler ABI that changes C2–C7 build on, and lands the riskiest
plumbing while every form still executes byte-identical imported leaves —
the most oracle-friendly point in the program.

## What Changes

- New internal handler ABI: `const gaby_vm::PredecodeCache::PredecodedEntry*
  (*)(vixl::aarch64::Simulator*, const PredecodedEntry*)` — handlers are
  `static` member functions of `vixl::aarch64::Simulator` in gaby marker
  blocks (direct access to protected leaves, free-function ABI). In C1 the
  return value is always `nullptr` (reserved; C2 defines the continuation
  protocol) and the harness keeps walking `pc_` exactly as today.
- Macro-generated generic thunks from the visitor list: each thunk seats
  `form_hash_`, runs the BTI gate, the MOVPRFX latch protocol, the
  statically-bound leaf call (`sim->Simulator::VisitXxx(...)` — devirtualized
  by qualification), the post-leaf MOVPRFX check, `last_instr_`/IncrementPc,
  and the trace/BType gated epilogue — byte-equivalent to today's
  `ExecuteInstructionCached` sequence.
- Sentinel handlers replace the sentinel pmfs (unimplemented / data-in-stream).
- `ResolvePredecodeHandler(form_hash)` beside `ResolvePredecodeLeaf`; the
  populate pass stores handler pointers in `entry->leaf`.
- `ExecuteInstructionCached` shrinks to entry lookup + one indirect handler
  call. `StepOnce` externally unchanged; decoder track untouched.
- Re-profile applogic at branch tip first and record the dispatch-hub share
  on the record (abort/re-price if materially below ~20%).

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `predecode-cache`: amend the frozen-entry-layout scenario's wording — the
  `leaf` slot now holds the resolved *handler* function pointer (size,
  offsets, field types unchanged); amend the type-erasure scenario to name
  the single-indirection dispatch.

## Impact

- `Sources/gaby_vm/src/aarch64/simulator-aarch64.h/.cc` (marker blocks:
  handler typedef/contract, thunk macro expansion, ResolvePredecodeHandler).
- `Sources/gaby_vm/src/gaby_vm/predecode_cache.cc` (populate pass, sentinels);
  `include/gaby_vm/predecode_cache.h` (leaf-slot comment wording).
- `docs/refs/gaby-vm-predecode-cache-design.md`: supersession notes (D8 thunk
  deferral; the "already fast enough" stance).
- Guard rails unchanged: vixl_port, full ctest, `bench_business --verify`,
  3-run before/after per shape (before = T5 row).

## Out of scope

- Threading/musttail, step-mode gate, boundary sentinels, null-PC terminal
  protocol (C2); specialization (C3+); operand64 plane (C4); FP handlers
  (C5); register residency (C6); fusion (C7).
