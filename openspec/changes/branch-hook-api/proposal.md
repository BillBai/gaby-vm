## Why

gaby-vm's V1 simulator currently has no way for an embedder to react to a
guest branch that targets host code. The cache track aborts when PC
leaves every registered code range; the decoder track happily decodes
whatever bytes happen to live at the target address — which for any
real host function (`objc_msgSend`, `malloc`, `dispatch_async`, an
embedder's own C++ function being patched) means the simulator either
crashes or executes nonsense.

This blocks the project's primary embedding scenario — an iOS
user-mode hot-fix engine that loads a guest Mach-O patch, runs it in
the simulator, and lets the patch transparently call back into the
host binary for everything the patch doesn't redefine. Today there
is no mechanism for the embedder to say "this branch target is a
host function, I will FFI-call it; this other branch target is
inside the patch I loaded, let the VM continue."

The embedder's branch classification is dynamic and target-dependent:
the typical implementation looks up the target address in a table of
loaded guest Mach-O ranges and dispatches as host if not found. So
the simulator must surface a per-branch callback with target address,
not a pre-registered "intercept these specific host symbols" list.

## What Changes

- Add `gaby_vm::Simulator::BranchHook` C-style function-pointer typedef
  in `include/gaby_vm/simulator.h`, with the signature
  `uintptr_t (*)(uintptr_t target_pc, void* user_data, Simulator& sim)`.
  The hook returns the address the simulator should commit as the next
  PC. `void* user_data` carries an embedder-owned opaque pointer so the
  hook can be a closure-equivalent without paying for `std::function`
  on the per-branch hot path. Termination is expressed by returning 0
  (the null-LR end-of-simulation sentinel) — the same path the guest
  uses when its own RET hits a null LR. No separate `BranchAction`
  return struct, no separate stop flag: the single `uintptr_t` return
  encodes the three outcomes (identity = return `target_pc`, divert =
  return some other in-range PC, terminate = return 0).
- Add `Simulator::SetBranchHook(BranchHook hook, void* user_data)` to
  install or replace the hook. Passing `nullptr` removes it. The
  installed hook applies to **both** the cache track and the decoder
  track — the embedder gets one observation point regardless of which
  execution mode is in use.
- Wire the hook into both tracks so it fires **before** the branch's
  PC update, on every PC-redirecting branch instruction the
  simulator executes (B, BL, BR, BLR, RET, and any taken conditional
  branch — `B.cond`, `CBZ`, `CBNZ`, `TBZ`, `TBNZ`). Not-taken
  conditionals fall through to PC+4 and do not invoke the hook.
- Cache-track integration lives in the cache execution path
  (`src/simulator.cc` and the per-branch leaf wrappers) and adds a
  null-check + indirect call to the hot loop. No `std::function`,
  no `std::variant`, no allocation per branch.
- Decoder-track integration lives in the imported VIXL branch visitor
  methods (`Simulator::VisitUnconditionalBranch*`,
  `VisitConditionalBranch`, `VisitTestBranch`, `VisitCompareBranch`,
  `VisitUnconditionalBranchToRegister` in
  `src/aarch64/simulator-aarch64.cc`) under marker blocks. The same
  hook pointer is shared by both tracks; the call point is the only
  per-track edit.
- When no hook is installed: current semantics preserved. Cache track
  aborts if PC leaves every registered range; decoder track keeps
  decoding (the existing behavior). The change is purely additive.
- Extend `demos/cli/main.cc` (currently a 69-line "simulator not yet
  wired" stub) into an end-to-end reference example: hand-assembled
  ARM64 bytes for a small function (an integer adder that calls a
  host `int host_add(int, int)` via `BL`), registered with a
  `PredecodeCache`, executed via `Simulator::RunFrom`, with a
  branch hook installed that recognises the host function address
  and dispatches via plain C ABI. The demo prints a deterministic
  result and serves as the canonical embedding pattern.
- Add tests under `test/`:
  - `branch_hook_dispatch_test.cc` — hook fires on each branch type,
    target address matches expectation, return-value paths (continue
    at target vs divert to a different PC vs terminate by returning 0)
    behave as documented.
  - `branch_hook_reentrancy_test.cc` — hook may call
    `Read`/`Write` on the simulator and seat a nested step via
    `RunFrom`/`StepOnce(entry_pc)`/`DebugStepOnce(entry_pc)`, the
    same re-entrancy contract `SetMemoryWriteObserver` already
    documents.
  - Extend `workload_shadow_test` to install a no-op hook (the
    "return target_pc unchanged" identity hook) and confirm the
    dual-track oracle still reports zero divergence. This pins
    down "hook installed but identity behaviour is invisible."

## Capabilities

### New Capabilities
<!-- None — the branch hook is an addition to the existing simulator
capability, not a new capability. -->

### Modified Capabilities

- `aarch64-simulator`: adds normative requirements for the per-branch
  hook API surface (the `BranchAction` value type, the `BranchHook`
  function-pointer typedef, the `SetBranchHook` entry point, the
  trigger contract — every PC-redirecting branch on both tracks,
  before PC update — and the no-hook behaviour preserves current
  semantics).

## Impact

- **Public API** (`include/gaby_vm/simulator.h`): additive only.
  New type `BranchAction`, new typedef `BranchHook`, new method
  `SetBranchHook`. No existing signatures change. ABI stability:
  RegisterFile and the existing typed Read/Write surface are
  untouched.
- **Cache-track execution path** (`src/simulator.cc` and adjacent
  internal headers): per-branch hot path gains one null-check on the
  hook pointer + one indirect call when set. With a null hook the
  overhead is one compare-and-branch (typically branch-predicted
  not-taken), measured well under 1 ns/insn at mixed-workload
  branch density.
- **Imported VIXL** (`src/aarch64/simulator-aarch64.cc`): branch
  visitor methods get marker-block edits to invoke the hook before
  updating PC. Marker blocks follow the existing convention; the
  reason text points back to this change. No leaf semantics change.
- **Tests**: two new test executables, plus a tweak to
  `workload_shadow_test`. The ctest expected count goes from 10 to
  at least 12.
- **Demo**: `demos/cli/main.cc` rewritten end-to-end. The CMake
  target shape doesn't change.
- **Bench**: new bench expectation — mixed cache with a null hook
  installed should stay within ~5% of the current 20.44 ns/insn
  baseline; with a typical "look up address in a small table" hook
  installed, mixed cache may move into the 21-23 ns/insn band. A
  formal acceptance gate is left to the change's tasks.md
  (calibrated against the post-B+C baseline).
- **Out of scope** for this change (will not be touched):
  - SVC / BRK / unimplemented instruction handling.
  - Memory write veto path (the project's hot-fix engine
    intentionally allows unrestricted host-memory writes by design).
  - Any change to `PredecodeCache` storage layout — the hook lives
    on `Simulator`, not on cache entries.
  - SwiftPM packaging — that's the sibling change `swiftpm-package`.
