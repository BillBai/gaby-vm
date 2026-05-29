## Context

gaby-vm's V1 simulator runs guest A64 code through two tracks (cache and
debug) that share the same imported VIXL leaf semantics. Both tracks
are closed loops today: once execution starts at an `entry_pc`, the
simulator marches through guest instructions until it terminates (a
`RET` to a null LR, the cache track's PC leaving every registered code
range, or whatever stop condition the debug track happens to hit).
There is no per-instruction observation point along the way for an
embedder to intervene.

The primary embedding scenario — an iOS user-mode hot-fix engine — needs
exactly one such observation point: every guest branch. A typical guest
patch starts in code we loaded, but its calls flow out to host functions
(`objc_msgSend`, `malloc`, the host binary's own functions) that we have
not loaded as guest code. The embedder needs the simulator to pause on
every PC-redirecting branch, classify the target ("inside my guest patch"
vs "in the host binary"), and either continue the simulator (guest
target) or dispatch a C-ABI FFI call (host target).

The existing observation surface — `SetMemoryWriteObserver` — covers
store events and supports a documented re-entrancy contract: the
observer may seat a nested step on the same Simulator via
`RunFrom`/`StepOnce(entry_pc)`/`DebugStepOnce(entry_pc)`. The branch
hook is the equivalent for branch events, reusing the same re-entrancy
machinery rather than inventing a new one.

Two structural facts shape the design.

1. **One leaf per branch family, shared by both tracks.** Each branch
   family — `B`/`BL`, `B.cond`, `TBZ`/`TBNZ`, `CBZ`/`CBNZ`,
   `BR`/`BLR`/`RET` and PAC variants — is implemented by exactly one
   VIXL visitor in `src/aarch64/simulator-aarch64.cc`. The debug track
   reaches each visitor through the imported decoder; the cache track
   reaches the same visitor through a pointer-to-member-function stored
   in `PredecodedEntry::leaf` and called from `ExecuteInstructionCached`
   in `src/aarch64/simulator-aarch64.h` (line 1587). **Editing the
   visitor once catches both tracks.** The proposal anticipated separate
   integration points for the two tracks, but inspection of the code
   shows the cache track does not duplicate the leaf — it dispatches
   through it.

2. **Branches are a meaningful fraction of dynamic mix.** A
   representative mixed workload runs around 15–25% branches by
   instruction count. Per-branch overhead multiplies through the run, so
   the hook ABI has to be cheap when no hook is installed (a single load
   plus a predictable null-check) and not much worse when one is (one
   C-ABI indirect call). The 20.44 ns/insn mixed-cache baseline is the
   number we don't want to move much.

The design lives within those two constraints. The rest of the choices
flow from them.

## Goals / Non-Goals

**Goals:**

- Surface a per-branch observation point that fires on every
  PC-redirecting branch, on both tracks, with one observation
  contract — the embedder does not need to know which track is
  executing.
- Null-hook overhead well under 1 ns/insn on top of the current
  ~20 ns/insn mixed-cache baseline.
- Installed-hook overhead in the 1–3 ns/insn band (one indirect call
  plus the embedder's body) — formalized to a regression budget in the
  change's `tasks.md`.
- C-ABI-clean signature so the hook ports trivially to the eventual
  Swift / C FFI layer (sibling change `swiftpm-package`).
- Identity hook (`return target_pc`) is observationally invisible:
  `workload_shadow_test` continues to report zero divergence with one
  installed. This pins down the "hook installed but identity behaviour"
  guarantee.
- Re-entrancy: the hook may call `Read`/`Write`/`RunFrom`/
  `StepOnce(entry_pc)`/`DebugStepOnce(entry_pc)` on the same Simulator
  with no cursor corruption, reusing the existing memory-write-observer
  re-entrancy contract.

**Non-Goals:**

- Per-target hook tables or range-keyed dispatch. The embedder owns
  dispatch inside its hook body — its classification is dynamic
  ("is this PC inside any of my loaded guest Mach-O ranges?") and an
  exact-match table inside the simulator doesn't help.
- Pre/post-execution hooks for non-branch instructions. Out of scope
  for V1; this change is branch-specific.
- SVC/BRK/HLT/unimplemented intercepts. Separate future change — the
  signature for "trap occurred" is meaningfully different from "branch
  to X" and should be designed on its own.
- Hook firing on not-taken conditional branches. A fall-through
  `PC+4` isn't a "branch took, here's where it's going" event.
- A branch-kind discriminator in the hook payload (no enum tag for
  `B` vs `BL` vs `BR`). If an embedder needs the kind, it can
  disassemble the instruction word at PC-4 — but no realistic use
  case in this change motivates the extra payload field.
- Branch-hook ABI stability across major library versions. The C
  function-pointer shape is FFI-friendly, but bumping the typedef in
  a future major version is allowed.

## Decisions

### D1. Hook signature: bare C function pointer + opaque `void*`

The hook typedef is:

```cpp
using BranchHook = uintptr_t (*)(uintptr_t target_pc,
                                 void* user_data,
                                 Simulator& sim);

void SetBranchHook(BranchHook hook, void* user_data);
```

Passing `nullptr` for `hook` removes it. `user_data` is opaque to the
simulator — it's the embedder's closure-equivalent context pointer,
passed through unchanged on every invocation.

The hook returns the address the simulator should commit as PC. The
three outcomes are all encoded by the return value:

- `return target_pc` — identity: continue at the architectural target.
- `return other_pc` — divert: commit a different PC.
- `return 0` — terminate the run. `WritePc(0)` lands on the existing
  `kEndOfSimAddress` sentinel; `IsSimulationFinished()` then returns
  true on the next `StepOnce` / `DebugStepOnce` and the enclosing run
  exits cleanly. This is the same termination path the guest uses with
  a `RET` to a null `LR` — no separate stop flag, no separate return
  struct, no special-case state.

**Why bare `uintptr_t` rather than a `BranchAction { uintptr_t pc; bool
stop; }` struct.** An earlier iteration of the design had `BranchAction`
with a `stop` flag, plus a `gaby_sim_finished_` field on the imported
simulator, plus an OR check in `StepOnce` / `DebugStepOnce`, plus a
`ClearGabyStop()` setter and four call sites in the gaby_vm wrappers.
The plumbing existed to preserve "after a stop, `Read(PC)` returns the
branch instruction's own PC" — a guarantee no real embedder needs (and
which costs the embedder nothing to recover by capturing the branch's
PC inside the hook itself, if they want it). Routing termination
through `kEndOfSimAddress` instead removes the field, the flag check,
the setter, and the four call sites — the hook fast path becomes one
load + one null-check + one indirect call + one `WritePc`.

**Why a bare function pointer over `std::function`.** Branches are
~20% of dynamic mix; the per-fire cost matters. A bare function
pointer compiles to one load + one indirect call. `std::function` adds
type erasure (a vtable inside the small-object buffer, possibly a heap
allocation for larger captures) and at least one additional indirect
call. The existing `SetMemoryWriteObserver` already uses
`std::function` because memory-write firing is a colder fraction of
the mix and the cost was acceptable there; for branches it isn't.

**Why `void*` user_data rather than relying on closures.** The bare
function pointer cannot capture context the way a `std::function` can.
`void*` user_data is the C-ABI substitute, and it's exactly what
non-C++ embedders (Swift through SwiftPM, eventual C consumers) would
have anyway. An embedder that wants a closure passes a pointer to its
context struct and casts inside the hook body.

**Alternative considered: virtual interface
(`class BranchHookInterface { virtual uintptr_t OnBranch(...) = 0; }`).**
Rejected. Same vtable cost as `std::function`, and it forces the
embedder to subclass — a closure-style hook (capturing an
embedder-side context object) becomes awkward.

**Alternative considered: keep `std::function`.** Re-examined because
the existing `MemoryWriteObserver` already uses it and consistency
would be nice. Rejected for the per-branch cost. The header still
includes `<functional>` for the memory-write observer, so we're not
saving an include either way, but we are saving cycles on the hot
path.

### D2. Single observation point in the VIXL leaves, both tracks pick it up

The hook invocation lives inside each of the five branch-leaf visitors
in `src/aarch64/simulator-aarch64.cc`:

- `VisitUnconditionalBranch` (`B`, `BL`)
- `VisitConditionalBranch` (`B.cond`) — only when `ConditionPassed`
- `VisitTestBranch` (`TBZ`, `TBNZ`) — only when `take_branch`
- `VisitCompareBranch` (`CBZ`, `CBNZ`) — only when `take_branch`
- `VisitUnconditionalBranchToRegister` (`BR`, `BLR`, `RET`, PAC
  variants `BRAA`/`BRAB`/...)

All five leaves share the same hook-fire logic — null-check, struct-return
handling, `stop`-vs-`divert` dispatch. That logic lives in **one shared
helper** on `vixl::aarch64::Simulator`, `GabyHookedWritePc(const Instruction*
target)`, defined under a marker block in `src/aarch64/simulator-aarch64.h`
(see D7 for the helper body and storage).

Each leaf's edit is then a **single-line redirect**: the existing
`WritePc(<target_expr>)` call becomes `GabyHookedWritePc(<target_expr>)`,
bracketed by a single-line `// gaby-vm:` marker citing this change. The
target expression each leaf passes is unchanged from upstream
(`instr->GetImmPCOffsetTarget()` for the four immediate-target leaves,
`Instruction::Cast(addr)` for `VisitUnconditionalBranchToRegister`).

**Why a shared helper rather than five inline if/else blocks.** Five copies
of a ~10-line null-check-plus-struct-dispatch block would mean any tweak to
the stop-vs-divert contract — for instance, switching the "mark finished"
mechanism, or adding an early-out for an identity action — needs to be
applied identically to five sites or risks divergence the shadow oracle
might or might not catch. Centralising the logic also keeps each leaf's
marker block to a single-line edit, which minimises the per-leaf surface a
future VIXL re-import has to rebase.

The cache track invokes these same visitors through its leaf
pointer-to-member-function table. Editing the leaves once catches
both tracks.

**Why one point and not a separate cache-track wrapper.** The
two-tracks-one-leaf invariant means any "wrapper layer" for the cache
track would either (a) duplicate the leaf into a parallel
branch-handling path, or (b) install a different leaf pointer for
branch entries in the predecode cache. Option (a) doubles the
maintenance surface for branch semantics and risks divergence the
shadow oracle would have to catch. Option (b) forces the predecode
pass to discriminate branch encodings, requires storing a
discriminator on every `PredecodedEntry`, and defeats the cache's
uniform leaf-signature design (`void (Simulator::*)(const Instruction*)`).
Neither buys anything observable to the embedder; the user-visible
behavior is the same regardless of where the hook call lives.

**Why not edit at the loop level instead.** A loop-level observer in
`gaby_vm::Simulator::Impl::RunFrom` cannot tell branch instructions
from non-branch ones without disassembling each instruction, which
defeats the cache. Putting the check at the leaf level uses the
existing dispatch to do the discrimination for free.

**Consequence for marker-block churn on VIXL re-imports.** The branch
visitors are stable VIXL code — they haven't changed shape in
upstream history we've seen. With the shared-helper design, each
re-import touches at most: one marker block in
`simulator-aarch64.h` (helper + fields + setters) and five
single-line redirects in `simulator-aarch64.cc`. The redirects are
the lowest-friction form a marker can take; the marker convention
documents the reason text pointing back at this change.

### D3. Hook fires after target resolution, before PC commit

The hook fires at the exact point in each leaf where the leaf has
computed the target address but has not yet called `WritePc`.
Concretely:

- For `B`/`BL`: after `GetImmPCOffsetTarget()`, and for `BL` after
  the LR write and `GCSPush`. The hook sees the new LR.
- For `B.cond`/`TBZ`/`TBNZ`/`CBZ`/`CBNZ`: only when the branch is
  taken (condition true / bit set as required / register zero or
  non-zero). The not-taken paths do not invoke the hook.
- For `BR`/`BLR`/`RET` (and PAC variants): after PAC authentication,
  after GCS check, after the optional LR write for the BL-class. The
  hook sees the architectural target (PAC bits stripped) and the
  post-branch LR. Importantly, this is also AFTER the
  `BranchInterception` path inside VIXL's
  `VisitUnconditionalBranchToRegister` (`simulator-aarch64.cc:4067`);
  if an interception fires, it has already run by the time the hook
  sees the target, and `addr` has been rewritten to LR by the
  interception path. The branch hook gets the "what address would
  we actually branch to next" view in all cases.

**Why after target resolution.** An FFI-dispatching embedder needs to
classify the resolved target. For PAC-signed targets, the resolved
target is the stripped one — the raw signed value is an implementation
detail.

**Why after LR write for BL/BLR.** The typical embedder's "do the
FFI call and return to caller" reaction is to read LR (now equal to
the return address) and `return LR` from the hook. If the hook fired
before LR was written, the embedder would see the stale LR from
before the branch and have to compute the return address from the
call-site PC itself.

**Why before PC commit.** The hook decides what address to write to
PC. Everything else (LR, condition flags, register file,
authentication state) should already reflect the architectural
post-branch state. The hook's return value goes straight into
`WritePc`: if it is the architectural target, execution continues
normally; if it is some other in-range address, execution diverts; if
it is 0, the simulator lands on `kEndOfSimAddress` and the next
`StepOnce` returns false.

### D4. Return type: bare `uintptr_t` (no `BranchAction` struct)

The hook returns the next PC. That single `uintptr_t` covers all three
outcomes:

- Return `target_pc` — identity: continue at the architectural target.
- Return `other_pc` — divert: continue at a different in-range PC.
  Cache track aborts on the next step with the existing "PC not in
  any registered code range" diagnostic if `other_pc` is outside
  every registered range.
- Return 0 — terminate: `WritePc(0)` lands on the existing
  `kEndOfSimAddress` sentinel, and `IsSimulationFinished()` returns
  true on the next `StepOnce` / `DebugStepOnce`. Same termination
  path as a guest `RET` to a null `LR`.

**Why bare `uintptr_t` rather than a `BranchAction { uintptr_t pc;
bool stop; }` struct.** The struct version added a second outcome
encoding (`stop = true`) for "terminate cleanly with PC left at the
branch instruction's own PC". Implementing that demanded a separate
`gaby_sim_finished_` flag on the imported simulator, an OR check in
`StepOnce` / `DebugStepOnce`, a `ClearGabyStop()` setter, and four
matching calls in the `gaby_vm` entry-point wrappers. None of that
machinery existed to enable a behaviour the embedder couldn't reach
otherwise — `return 0` already terminates the run cleanly through the
existing `kEndOfSimAddress` path. The only difference is `Read(PC)`
after termination: with the struct version it was the branch
instruction's own PC; with bare `uintptr_t` it is 0 (the sentinel,
which doubles as the conventional "run finished" marker). No realistic
embedder is paying attention to PC immediately after a self-requested
termination; on the rare occasion they want it, they can capture it
inside the hook body before returning 0. We took the simplification.

**Why `uintptr_t` and not `const Instruction*`.** The public header
must not leak VIXL types. `uintptr_t` is the same width and the
embedder's classification is by address anyway.

**Future-extension shape.** If a later change needs the hook to
communicate additional state (e.g., a side-channel notification that
the embedder wants the simulator to log), the API can grow in either
direction without breaking existing hooks: add another parameter to
`BranchHook` (source-compatible if the caller defaults the new arg),
or introduce a sibling `BranchHook2` typedef that returns a struct
with the additional fields. The current shape doesn't preclude either
move.

### D5. One hook per Simulator, not a target-keyed table

`SetBranchHook` installs or replaces a single function pointer +
user_data on this Simulator. There is no built-in target → handler
dispatch table.

**Why.** The embedder already has a classification table — its
table of loaded guest Mach-O ranges. The hook body does:

```cpp
uintptr_t MyHook(uintptr_t target, void* ud, Simulator& sim) {
  auto* self = static_cast<MyContext*>(ud);
  if (self->is_in_loaded_guest(target)) {
    return target;  // continue in the simulator
  }
  // host target — read X0..X7 as args, do the call, write result
  // to X0, return to caller.
  self->dispatch_host(target, sim);
  return sim.Read(GpRegister::LR);
}
```

A built-in exact-match `pc → handler` table doesn't fit this dispatch
shape: the embedder's classification is range-based, not exact-match.
Pushing the table down into the simulator would force the embedder to
flatten its dispatch into per-PC entries, double the bookkeeping cost
(one entry per loaded function rather than one entry per loaded
range), and require us to design and maintain the table data
structure (lookup invariants, thread-safety, lifetime).

The imported VIXL simulator has an internal
`MetaDataDepot::BranchInterception` mechanism (used inside
`VisitUnconditionalBranchToRegister` at `simulator-aarch64.cc:4067`)
that does exact-match BLR-only dispatch. It exists for VIXL's own
tests; it doesn't generalize to the embedder's range-based needs and
we don't extend it.

**Alternative considered: hook + optional target-PC filter
(`SetBranchHook(hook, ud, filter_pc_min, filter_pc_max)`).** Rejected
as premature. If branch-hook firing frequency turns out to be a real
perf problem, we can add a filter later in an additive way. The API
stays additive either direction.

### D6. Re-entrancy contract — same as `MemoryWriteObserver`

From inside the hook, the embedder may:

- Call `sim.Read(reg)` / `sim.Write(reg, value)` for all typed
  register I/O **except** `Write(GpRegister::PC, …)`.
- Seat a nested step via `sim.RunFrom(entry_pc)`,
  `sim.StepOnce(entry_pc)`, or `sim.DebugStepOnce(entry_pc)`.

The embedder must NOT:

- Call `sim.Write(GpRegister::PC, …)` followed by bare
  `sim.StepOnce()` / `sim.DebugStepOnce()`. The bare PC write
  mutates the PC outside the re-entrancy guard and corrupts the
  enclosing run's cursor.
- Call `sim.WriteAll(...)`. Same reason — `WriteAll` is a top-level
  state-restore entry point and mutates the PC outside the guard.

**Why this works for free.** The existing `ExecutionScope` (in
`src/gaby_vm/simulator.cc`, lines 80–114) saves/restores the imported
simulator's interpreter cursor on nested entry via the
`GabySaveCursor`/`GabyRestoreCursor` member functions on
`vixl::aarch64::Simulator` (`simulator-aarch64.h:1662-1666`). The
branch hook fires from inside a leaf, which is inside an outer
`ExecuteInstructionCached` (cache track) or `ExecuteInstruction`
(debug track), which is inside an outer `RunFrom`/`StepOnce`
`ExecutionScope`. A re-entrant `RunFrom` from the hook enters as a
nested `ExecutionScope`: the cursor is snapshotted on entry, the
inner run executes, returns, and the cursor is restored on exit. This
is the same mechanism that already makes nested re-entry from a
memory-write observer correct.

**Consequence.** No new re-entrancy machinery; the design just
documents that the existing contract extends to the branch hook.
The reentrancy test (`branch_hook_reentrancy_test.cc`) pins down the
working pattern and confirms the contract empirically.

**Why we don't add a runtime check for the disallowed paths.** A
guard that aborts on "bare PC write from inside a leaf" would need
to distinguish "I'm inside a nested call" (bad) from "I'm setting up
the next top-level run before its scope opens" (fine). The existing
`busy_` flag is set as soon as the outer scope opens, so a bare PC
write from inside a leaf does have `busy_` true — but so does a bare
PC write from inside the body of a `RunFrom` between scope-open and
the loop, where it's actually intended. A precise guard would need
more state than we want to add for a contract that's clearly
documented and that the typed-API docstring already calls out for the
memory-write observer.

### D7. Hook pointer storage on the imported `vixl::aarch64::Simulator`

Three new fields under a marker block in
`src/aarch64/simulator-aarch64.h`:

```cpp
// gaby-vm BEGIN: branch-hook-api
gaby_vm::Simulator::BranchHook gaby_branch_hook_ = nullptr;
void* gaby_branch_hook_data_ = nullptr;
gaby_vm::Simulator* gaby_outer_sim_ = nullptr;
// gaby-vm END
```

plus two setters and the shared hook helper (all in the same marker
block):

```cpp
void SetGabyBranchHook(gaby_vm::Simulator::BranchHook hook, void* data) {
  gaby_branch_hook_ = hook;
  gaby_branch_hook_data_ = data;
}
void SetGabyOuterSim(gaby_vm::Simulator* outer_sim) { gaby_outer_sim_ = outer_sim; }

// The single shared invocation point for all five branch leaves.
// Replaces the upstream `WritePc(target)` call in each leaf so the
// hook contract is defined in exactly one place. When no hook is
// installed, behaviour is identical to upstream. The hook returns
// the next PC; 0 lands on kEndOfSimAddress and terminates the run on
// the next StepOnce.
void GabyHookedWritePc(const Instruction* target) {
  if (gaby_branch_hook_ == nullptr) {
    WritePc(target);
    return;
  }
  const uintptr_t next_pc = gaby_branch_hook_(
      reinterpret_cast<uintptr_t>(target),
      gaby_branch_hook_data_,
      *gaby_outer_sim_);
  WritePc(reinterpret_cast<const Instruction*>(next_pc));
}
```

Each of the five branch-visitor leaves then becomes a one-line edit:
`WritePc(target_expr)` → `GabyHookedWritePc(target_expr)`, bracketed by
a single-line `// gaby-vm:` marker. The null-check, the hook
invocation, and the `WritePc` of the returned address all live in this
helper and nowhere else.

`gaby_outer_sim_` is set once in `gaby_vm::Simulator::Impl`'s constructor
(`vsim.SetGabyOuterSim(this->outer_)`) and never changes for the
Simulator's lifetime — it's the back-pointer the leaves (transitively,
the helper) need to pass `gaby_vm::Simulator&` to the hook.

**Why the helper lives on `vixl::aarch64::Simulator` and not on
`gaby_vm::Simulator`.** It needs to call the upstream `WritePc`, which is
a protected member of `vixl::aarch64::Simulator`. Co-locating it with
the fields (same class, same marker block) gives it the access it needs
without a `friend` declaration or a public-`WritePc` workaround.

**Why the helper is defined inline in the header rather than
out-of-line in the `.cc`.** Inlining keeps the call sites a direct
member-function invocation; the compiler can fold the null-check past
each five-leaf entry without an LTO assumption. The body is short
(~10 lines), well within what we'd inline anyway.

**Why three fields rather than a sink interface.** `MemoryWriteSink`
in VIXL solves the same gaby_vm-types-in-leaves problem via vtable
indirection — the sink interface is in VIXL, the gaby_vm side
subclasses it (see `ForwardingWriteSink` in
`src/gaby_vm/simulator.cc`, lines 119–131). For the branch hook we
do not use a sink because each branch costs an indirect call already
(via the function pointer); routing through a virtual method would
double the per-fire indirection (vtable load + virtual call) without
buying us anything — the hook signature is a single `uintptr_t`
return that fits in a register on AArch64. Direct function-pointer
storage is the simplest answer that meets the perf goal.

**Why all three fields rather than a single struct.** Two of the
three (`gaby_branch_hook_`, `gaby_branch_hook_data_`) change together
on `SetBranchHook`; the third (`gaby_outer_sim_`) is set once at
construction. Packing them into one struct doesn't simplify anything
and complicates the marker block.

### D8. Threading — single-writer, like the rest of the Simulator

`SetBranchHook` is not thread-safe relative to a concurrent execution
call on the same Simulator. The embedder must install the hook before
starting execution, or stop execution before changing it. This
matches the existing Simulator threading contract documented in
`include/gaby_vm/simulator.h`: the Simulator is a single-threaded
object.

**Why no atomic loads on the hot path.** An atomic load with acquire
semantics on Apple Silicon is cheap but not free, and the embedder is
not trying to swap hooks during a run. The cost would buy a contract
no realistic embedder needs.

## Risks / Trade-offs

**[R1] Null-hook hot-path regression.** Adding one load + one
predicted-not-taken branch per branch instruction has a real but
small cost. With branches at ~20% of dynamic mix and one cycle for
the load on Apple Silicon, we expect ~0.2 ns/insn overhead, well
inside the proposal's 5% budget on the 20.44 ns/insn baseline.
→ **Mitigation:** the bench harness's mixed-cache run gets a
"null-hook installed" variant; the change's `tasks.md` pins a
regression budget against the post-B+C baseline.

**[R2] Installed-hook hot-path cost.** With a hook installed, every
branch pays one indirect C-ABI call plus the embedder's body. For a
realistic embedder hook (small range-lookup table), 21–23 ns/insn
mixed cache is the expected band. → **Mitigation:** documented up
front; the formal acceptance gate lands in `tasks.md` once the post-
B+C baseline is measured.

**[R3] Diverted PC lands outside any registered cache range.** A
hook returning a `foo` PC outside every registered range causes the
cache track to abort on the next step.
→ **Mitigation:** this IS the existing cache-track range contract;
the design just documents that the branch hook does not exempt the
embedder from it. The existing diagnostic ("PC not in any registered
code range") is already specific.

**[R4] Re-entrant hook corrupts the enclosing run.** An embedder
that calls `Write(GpRegister::PC, …) + StepOnce()` from inside the
hook instead of `StepOnce(entry_pc)` silently corrupts the enclosing
run's cursor on resume. → **Mitigation:** the typed-API docstring
already calls out the rule for memory-write observers; the
BranchHook docstring reuses the same wording and points to the same
rule. The reentrancy test pins down the working pattern. No runtime
guard for the reasons in D6.

**[R5] PAC-signed target is stripped before the hook sees it.** An
embedder that wants to inspect the raw signed value cannot — it sees
the authenticated (stripped) address. → **Trade-off:** the raw
value is an implementation detail of PAC; FFI dispatch needs the
architectural address. We accept this trade.

**[R6] Hook fires on RET, possibly surprising embedders that only
think of "function entry."** An embedder hooking "every function
entry" by hooking BL also gets RET fires, which it must filter or
accept. → **Trade-off:** filtering RET inside the hook is trivial
(compare `target_pc` against a known guest-LR value or the
"interpreter sentinel" RET-to-null-LR). Surfacing branch kind in the
hook payload is rejected (D4 / Non-Goals). Discriminating by
disassembling PC-4 is available to embedders that genuinely need it.

**[R7] Five marker-block visitor edits will need re-applying on a
VIXL re-import.** → **Mitigation:** the marker convention in
`docs/architecture.md` gives a canonical reason text pointing back at
this change name. The edits are small and isolated; `grep gaby-vm`
finds every site.

**[R8] Marker block on `vixl::aarch64::Simulator` adds three new
fields that contend for cache line space.** The hook pointer is read
on every branch; placement matters for cache locality. → **Trade-off:**
we place the marker block near `pc_modified_` / `form_hash_` /
`last_instr_` so the hook pointer shares a cache line with hot
interpreter state. If profiling later shows a placement-driven
regression, we can adjust within the marker block.

## Migration Plan

This is purely additive; no existing API changes, no existing
behavior changes when no hook is installed.

1. **Public header (`include/gaby_vm/simulator.h`).** Add
   `BranchHook` typedef (returning `uintptr_t`) and `SetBranchHook`
   method. Document re-entrancy contract by pointing to the
   existing memory-write-observer rule.
2. **Imported VIXL (`src/aarch64/simulator-aarch64.h` and `.cc`).**
   Marker block in the header adds three fields + two setters.
   Five marker-block patches in the .cc edit the branch visitors.
3. **gaby_vm internals (`src/gaby_vm/simulator.cc`).** Wire the
   back-pointer in `Impl`'s constructor; implement
   `Simulator::SetBranchHook` by forwarding to
   `vsim.SetGabyBranchHook`.
4. **Tests (`test/`).** Add `branch_hook_dispatch_test.cc` and
   `branch_hook_reentrancy_test.cc`. Extend `workload_shadow_test`
   to install an identity hook and confirm zero divergence.
5. **Demo (`demos/cli/main.cc`).** Rewrite end-to-end: hand-assembled
   bytes for a small guest function calling a host
   `int host_add(int, int)` via `BL`, registered with a
   `PredecodeCache`, executed via `Simulator::RunFrom`, with a hook
   installed that recognises the host function address and
   dispatches via plain C ABI. The demo prints a deterministic
   result and serves as the canonical embedding pattern.
6. **Bench (`bench/`).** Add a "mixed cache, null hook installed"
   measurement variant. Add a separate "identity hook installed"
   variant. The acceptance gate against the 20.44 ns/insn baseline
   goes in `tasks.md`.

Rollback: revert all six steps together. The change is small
enough that there is no partial rollback path.

## Open Questions

**[OQ1] Should the hook receive a branch-kind tag (a fourth
argument)?** No for V1. The embedder can disassemble PC-4 if it
needs to know. Revisit if a real use case demands it; the hook
signature can grow source-compatibly by adding an argument with a
default value (see D4's "future-extension shape" note).

**[OQ2] Should SVC/BRK/HLT/unimplemented have their own hook?** Out
of scope for this change. Likely future work, but the signature for
"trap occurred" is different enough from "branch to X" that it
deserves its own design pass.

**[OQ3] Should the hook be invocable from the eventual C / Swift FFI
layer?** The function-pointer ABI is mechanical to forward — a C-API
would expose
`gaby_vm_set_branch_hook(uintptr_t (*)(uintptr_t, void*, void*),
void*)` where the third parameter is an opaque Simulator handle.
That's the sibling `swiftpm-package` change's territory; the
`BranchHook` typedef is designed to be C-ABI-clean so the forwarding
is trivial when we get there.

**[OQ4] Should the workload_shadow oracle gain a "branch event
divergence" check?** Today the oracle compares per-instruction
memory writes between tracks. With identity-hook installed, every
branch on both tracks fires the same hook with the same target — so
extending the oracle to compare branch-event sequences across tracks
is a natural next step. Not in scope here; recorded as a candidate
follow-up so the empty-hook identity test doesn't slip into "we
forgot to actually check this."
