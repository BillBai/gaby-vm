# Proposal: cache-threaded-runfrom

## Why

C2 of the fast-dispatch plan
([`docs/refs/gaby-vm-fast-dispatch-synthesis-2026-07-03.md`](../../../docs/refs/gaby-vm-fast-dispatch-synthesis-2026-07-03.md)):
with C1's handler ABI in place, the remaining per-instruction dispatch cost
is the central loop itself — call/ret around every handler, the per-step
range containment check, and a single shared indirect-call site that
denies the branch predictor per-form history. Threading the handlers
(`[[clang::musttail]]` handler→handler chains) removes all three. This is
the "thread code" direction requested on 2026-07-03.

## What Changes

- Handlers compute a real continuation: straight-line → `entry + 1`
  (range check hoisted to a per-range boundary sentinel); PC-modifying
  leaves → re-derive from `pc_` (null PC → terminal handler, synthesis H2;
  outside all ranges → hard abort, unchanged).
- `GABY_DISPATCH` macro: musttail tail-call in threaded builds, plain
  return in the loop fallback (`GABY_THREADED=0`, for gcc<15 /
  sanitizers), with the **runtime step-mode gate** (synthesis H1) so one
  binary hosts both the chaining and single-stepping representations —
  ShadowRunner and `StepOnce` execute the *same compiled handler bodies*
  one instruction at a time.
- Predecode appends one boundary-sentinel entry per range
  (`entries[size/4]`), preserving cross-range straight-line fallthrough
  and the abort-on-unregistered-PC contract (synthesis H3).
- `gaby_vm::Simulator::RunFrom` becomes the threaded harness;
  `StepOnce`/ShadowRunner keep per-instruction semantics via the gate;
  decoder track untouched.
- New equivalence tests (threaded vs stepped vs decoder whole-run;
  hook-in-chain nested re-entry; branch-to-unregistered abort; adjacent
  -range fallthrough) and CI coverage for the `-O0` threaded build and
  the `GABY_THREADED=0` fallback.

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `predecode-cache`: add threaded-execution requirement (indistinguishable
  from stepped execution; step-mode gate); boundary-sentinel wording for
  the entries array (`size/4 + 1`); a re-entrancy scenario clarifying the
  continuation is frame-local and `pc_` is authoritative at hook
  boundaries (no cursor-set change).

## Impact

- `simulator-aarch64.h/.cc` (thunk epilogue continuation, dispatch macro,
  step-mode member, terminal/boundary handlers — gaby marker blocks);
- `gaby_vm/simulator.cc` (ExecutionScope step-mode save/set/restore,
  RunFrom); `predecode_cache.cc/.h` (entries +1, boundary sentinel,
  CodeRange comment); `test/` (new equivalence + re-entry + abort tests);
- `ci/` + workflow (threaded `-O0` vixl_port job, `GABY_THREADED=0` build);
- `docs/refs/gaby-vm-predecode-cache-design.md` (supersede the
  direct-threading non-goal).

## Out of scope

Specialized handlers (C3+), operand64 plane (C4), FP handlers (C5),
register residency / `preserve_none` (C6), fusion (C7).
