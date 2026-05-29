## ADDED Requirements

### Requirement: Public branch-hook typedef is declared in `include/gaby_vm/simulator.h`

`include/gaby_vm/simulator.h` SHALL declare the typedef

```cpp
using BranchHook = uintptr_t (*)(uintptr_t target_pc, void* user_data,
                                 Simulator& sim);
```

inside the `gaby_vm::Simulator` class scope.

The `BranchHook` typedef MUST be a bare function pointer; it SHALL NOT be a
`std::function<...>`, a pointer-to-virtual-member type, or any other
type-erased callable. This keeps the per-branch hot path at one load plus
one indirect call when a hook is installed.

The declaration of `BranchHook` SHALL NOT include any imported VIXL header
and SHALL NOT reference any `vixl::*` symbol, per the existing "Public
header surface does not expose VIXL types" requirement.

#### Scenario: BranchHook is a bare function-pointer typedef returning uintptr_t

- **WHEN** the declaration of `gaby_vm::Simulator::BranchHook` in
  `include/gaby_vm/simulator.h` is inspected
- **THEN** it is a function-pointer typedef with the signature
  `uintptr_t (*)(uintptr_t, void*, Simulator&)`
- **AND** it is NOT `std::function<...>` or a pointer-to-virtual-member type

#### Scenario: Branch-hook typedef does not leak VIXL identifiers

- **WHEN** `git grep -nE 'vixl|aarch64/' include/gaby_vm/simulator.h` is run
  against the lines that declare `BranchHook`
- **THEN** no `vixl::*` symbol or imported-header path appears in that
  declaration

### Requirement: `Simulator::SetBranchHook` installs, replaces, or removes the hook

`gaby_vm::Simulator` SHALL expose the public member function

```cpp
void SetBranchHook(BranchHook hook, void* user_data);
```

Calling `SetBranchHook` with a non-null `hook` SHALL install (or atomically
replace) the active hook and its associated `user_data` pointer. Calling
`SetBranchHook(nullptr, _)` SHALL remove any installed hook; the `user_data`
argument is then ignored.

After installation, the hook SHALL apply to **both** the cache execution
track and the decoder execution track. The embedder SHALL NOT need to
distinguish which track is active.

`user_data` is opaque to the simulator: the simulator SHALL pass the same
pointer value, unchanged, as the `user_data` argument on every subsequent
invocation of the hook.

`SetBranchHook` is not thread-safe relative to a concurrent execution call
on the same `Simulator`. The embedder MUST install or replace the hook
either before any execution call begins on this `Simulator`, or after the
current execution call has returned. This matches the existing single-writer
threading contract documented for `gaby_vm::Simulator`.

#### Scenario: SetBranchHook installs a hook that fires on subsequent branches

- **WHEN** an embedder calls `sim.SetBranchHook(MyHook, &ctx)` before any
  execution begins, then runs guest code containing at least one branch via
  `sim.RunFrom(entry_pc)`
- **THEN** `MyHook` is invoked at least once during the run, with its
  `user_data` argument equal to `&ctx`

#### Scenario: Passing nullptr removes the hook

- **WHEN** an embedder calls `sim.SetBranchHook(MyHook, &ctx)`, then later
  (between top-level execution calls) `sim.SetBranchHook(nullptr, nullptr)`,
  then runs guest code containing branches
- **THEN** `MyHook` is not invoked during the subsequent run

#### Scenario: Replacing a hook takes effect on the next branch

- **WHEN** an embedder calls `sim.SetBranchHook(HookA, &ctxA)`, then later
  (between top-level execution calls) `sim.SetBranchHook(HookB, &ctxB)`,
  then runs guest code containing branches
- **THEN** `HookB` is invoked with `user_data == &ctxB` and `HookA` is not
  invoked during the subsequent run

### Requirement: Branch hook fires on every taken PC-redirecting branch, after target resolution, before PC commit

When a hook is installed, the simulator SHALL invoke it exactly once for
every taken PC-redirecting branch the guest executes, on both the cache
track and the decoder track. The branch families covered are:

- Unconditional immediate: `B`, `BL`.
- Conditional immediate: `B.cond` — only when the condition passes.
- Test branch: `TBZ`, `TBNZ` — only when the branch is taken.
- Compare branch: `CBZ`, `CBNZ` — only when the branch is taken.
- Register-indirect: `BR`, `BLR`, `RET`, and the PAC variants `BRAA`,
  `BRAB`, `BLRAA`, `BLRAB`, `RETAA`, `RETAB`.

The hook SHALL fire **after** target resolution and **before** PC commit.
Specifically, by the time the hook body runs:

- The architectural target address has been resolved: for PAC variants the
  authenticator has run and the returned `target_pc` SHALL be the
  architectural (stripped) address; for register-indirect branches the GCS
  check has run; for immediate branches the immediate offset has been added
  to the branch instruction's PC.
- For BL-class branches (`BL`, `BLR`, `BLRAA`, `BLRAB`), LR SHALL already
  hold the post-branch return address (the instruction following the branch).
  The hook SHALL observe this updated LR via `sim.Read(GpRegister::LR)`.
- PC SHALL NOT yet have been committed; the simulator chooses what to do
  with PC from the hook's return value.

Both tracks SHALL invoke the same installed hook with the same `target_pc`
argument for the same dynamic branch.

Not-taken conditional, test, or compare branches (those that fall through
to `PC+4`) SHALL NOT invoke the hook. A fall-through to `PC+4` is not a
branch event in this contract.

#### Scenario: B fires the hook with the immediate target

- **WHEN** a guest executes `B label` where the assembled target is `0x1100`
- **THEN** the installed hook is invoked exactly once with
  `target_pc == 0x1100`

#### Scenario: BL fires the hook with LR already updated

- **WHEN** a guest executes `BL label` at PC `0x1000` (so the post-branch
  return address is `0x1004`)
- **THEN** the hook is invoked exactly once with `target_pc == label`
- **AND** when the hook reads `sim.Read(GpRegister::LR)`, it observes
  `0x1004`

#### Scenario: Conditional branch fires only when the condition passes

- **WHEN** a guest executes `B.eq label` with the Z flag clear (condition
  fails)
- **THEN** the hook is NOT invoked, and execution falls through to `PC+4`
- **WHEN** the same `B.eq label` executes with the Z flag set (condition
  passes)
- **THEN** the hook is invoked exactly once with `target_pc == label`

#### Scenario: TBZ / TBNZ fires only when the branch is taken

- **WHEN** a guest executes `TBZ Xn, #bit, label` with the tested bit value
  such that the branch is NOT taken
- **THEN** the hook is NOT invoked, and execution falls through to `PC+4`
- **WHEN** the same `TBZ Xn, #bit, label` executes with the tested bit
  value such that the branch IS taken
- **THEN** the hook is invoked exactly once with `target_pc == label`

#### Scenario: CBZ / CBNZ fires only when the branch is taken

- **WHEN** a guest executes `CBZ Xn, label` with `Xn != 0` (branch not
  taken)
- **THEN** the hook is NOT invoked, and execution falls through to `PC+4`
- **WHEN** the same `CBZ Xn, label` executes with `Xn == 0` (branch taken)
- **THEN** the hook is invoked exactly once with `target_pc == label`

#### Scenario: BR fires the hook with the architectural target

- **WHEN** a guest executes `BR Xn` where `Xn` holds an authenticated
  address whose architectural value is `0x2000`
- **THEN** the hook is invoked exactly once with `target_pc == 0x2000`

#### Scenario: PAC-signed BRAA / RETAA target is delivered post-authentication

- **WHEN** a guest executes `BRAA Xn, Xm` where the authenticated target
  is `0x3000`
- **THEN** the hook is invoked exactly once with `target_pc == 0x3000`
  (the stripped, architectural value — not the raw signed value)

#### Scenario: Cache track and decoder track invoke the hook identically

- **WHEN** the same guest function containing one or more branches is
  executed once via the cache track and once via the decoder track, with
  the same installed hook
- **THEN** the hook is invoked the same number of times in both runs
- **AND** the `target_pc` argument matches on each corresponding invocation

### Requirement: Hook return value chooses between continue, divert, and terminate

The `uintptr_t` value the hook returns SHALL determine the simulator's
next observable state, as follows:

- **Continue at the architectural target** — return `target_pc` (the
  identity action). The simulator SHALL commit PC to `target_pc` exactly
  as the architectural branch would. With no other side effects in the
  hook body, this is observationally indistinguishable from running with
  no hook installed.
- **Divert to a chosen PC** — return a non-zero `other_pc` where
  `other_pc != target_pc`. The simulator SHALL commit PC to `other_pc`.
  The diverted PC is subject to the same constraints as any other PC: on
  the cache track, an `other_pc` that lies outside every registered code
  range SHALL cause the next `ExecuteInstructionCached` to abort with the
  existing "PC not in any registered code range" diagnostic.
- **Terminate the run** — return `0`. The simulator SHALL commit PC to
  `0` (the existing `kEndOfSimAddress` end-of-simulation sentinel). The
  next `StepOnce()` / `DebugStepOnce()` invocation SHALL return `false`
  (run-completed). This is the same termination path the guest takes via
  `RET` to a null `LR`; no separate stop flag and no separate state
  machine.

#### Scenario: Identity return commits the architectural target

- **WHEN** a hook returns `target_pc` from a branch whose architectural
  target is `target_pc`
- **THEN** after the branch leaf returns, `sim.Read(GpRegister::PC)` is
  `target_pc`

#### Scenario: Divert return commits the chosen PC instead of the architectural target

- **WHEN** a hook returns `other_pc` from a branch whose architectural
  target is `target_pc != other_pc`, with `other_pc` non-zero
- **THEN** after the branch leaf returns, `sim.Read(GpRegister::PC)` is
  `other_pc`

#### Scenario: Returning 0 terminates the enclosing run

- **WHEN** a hook returns `0` during a `sim.RunFrom(entry_pc)` invocation
- **THEN** the enclosing `RunFrom` returns
- **AND** an immediately subsequent `sim.StepOnce()` returns `false`

#### Scenario: Diverted PC outside any registered cache range surfaces the existing diagnostic

- **WHEN** a hook returns a non-zero `bogus_pc` that lies outside every
  range registered with the `PredecodeCache`, and the cache track attempts
  to execute the next instruction
- **THEN** the cache track aborts with the existing "PC not in any
  registered code range" diagnostic
- **AND** the diagnostic identifies `bogus_pc` as the unmapped PC

### Requirement: Null-hook behavior preserves the pre-change observable contract

The simulator's observable behavior SHALL be identical to its pre-change
behavior whenever no hook is installed — that is, when `SetBranchHook`
has never been called on this `Simulator`, or when its most recent call
was `SetBranchHook(nullptr, _)`. Specifically:

- The cache track SHALL terminate the run when PC leaves every registered
  code range, with the existing diagnostic.
- The decoder track SHALL continue decoding whatever bytes lie at the
  current PC, as before.
- The per-branch hot path SHALL pay at most a load and a predicted
  null-check on a function-pointer field; no hook function SHALL be called.

An installed **identity** hook — one whose body is `return target_pc;`
with no other observable side effects — SHALL be observationally
invisible. Specifically, the `workload_shadow_test` oracle SHALL report
zero divergence between the cache track and the decoder track when an
identity hook is installed and every committed bench workload is
executed.

#### Scenario: Cache track with no hook still aborts on out-of-range PC

- **WHEN** a guest branch sends PC outside every registered code range,
  with no hook installed
- **THEN** the cache track aborts with the existing "PC not in any
  registered code range" diagnostic, exactly as before this change

#### Scenario: Identity hook is observationally invisible

- **WHEN** the `workload_shadow_test` oracle runs every committed bench
  workload against both the cache track and the decoder track, with an
  identity hook (`return target_pc;`) installed on the simulator
- **THEN** the oracle reports zero divergence across all workloads and all
  observable simulator state (general-purpose registers, vector registers,
  flags, memory)

### Requirement: Branch hook may re-enter the simulator under the documented contract

The branch hook SHALL be subject to the same re-entrancy contract that
the existing memory-write observer follows. From inside the branch hook
body, the embedder MAY perform any of the following on the same
`gaby_vm::Simulator` instance:

- Call any typed `sim.Read(...)` overload.
- Call any typed `sim.Write(...)` overload **except** `sim.Write(GpRegister::PC, _)`.
- Seat a nested run or step via `sim.RunFrom(entry_pc)`,
  `sim.StepOnce(entry_pc)`, or `sim.DebugStepOnce(entry_pc)`.

The embedder MUST NOT, from inside the hook body, do either of the
following:

- Call `sim.Write(GpRegister::PC, _)` followed by bare `sim.StepOnce()` or
  `sim.DebugStepOnce()`. A bare PC write from inside a leaf mutates the
  simulator's PC outside the re-entrancy guard and corrupts the enclosing
  run's cursor on resume. This is the same rule the existing
  "Simulator exposes typed register Read/Write accessors" requirement
  already documents for `Write(GpRegister::PC, _)`.
- Call `sim.WriteAll(_)`. `WriteAll` is a top-level state-restore entry
  point and mutates PC outside the guard for the same reason; this is the
  rule the existing "ReadAll / WriteAll snapshot and restore the full
  guest state" requirement already documents.

The docstring attached to `Simulator::SetBranchHook` in
`include/gaby_vm/simulator.h` SHALL state this re-entrancy contract
explicitly. It SHALL name the three re-entrant entry points (`RunFrom`,
`StepOnce(entry_pc)`, `DebugStepOnce(entry_pc)`) and SHALL identify the
two disallowed patterns (bare `Write(GpRegister::PC, _)` followed by
`StepOnce()` / `DebugStepOnce()`, and `WriteAll`).

#### Scenario: Hook reads and writes architectural register state

- **WHEN** a hook body reads `sim.Read(GpRegister::X0)`, computes a new
  value, writes it back via `sim.Write(GpRegister::X0, new_value)`, and
  then returns `target_pc`
- **THEN** after the hook returns, instructions executed in the enclosing
  run observe `new_value` in `X0`

#### Scenario: Hook seats a nested run via RunFrom

- **WHEN** a hook body invokes `sim.RunFrom(nested_entry_pc)` for a
  guest function arranged to terminate cleanly (for example via a `RET`
  to a null LR), and then returns `resumed_pc`
- **THEN** the nested `RunFrom` returns cleanly
- **AND** the enclosing run resumes execution at `resumed_pc` without
  cursor corruption

#### Scenario: SetBranchHook docstring documents the re-entrancy contract

- **WHEN** the docstring attached to
  `gaby_vm::Simulator::SetBranchHook` in `include/gaby_vm/simulator.h`
  is read
- **THEN** it states that the hook MAY call any typed `Read`, any typed
  `Write` other than `Write(GpRegister::PC, _)`, `RunFrom`,
  `StepOnce(entry_pc)`, and `DebugStepOnce(entry_pc)` on the same
  `Simulator`
- **AND** it states that the hook MUST NOT call bare
  `Write(GpRegister::PC, _)` followed by `StepOnce()` /
  `DebugStepOnce()`, and MUST NOT call `WriteAll`
