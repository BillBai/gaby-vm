## Context

`gaby_vm::Simulator` today exposes guest register state through a flat set of
per-register accessors, all keyed by raw `unsigned` codes:
`WriteXRegister(unsigned, uint64_t)`, `ReadVRegister(unsigned)`, plus bespoke
`WriteSp`/`ReadSp` and `WritePc`/`ReadPc`. System registers (NZCV, FPCR, FPSR,
BType) are read-only. There is no batch entry point.

The proposal replaces this surface with a strongly-typed, enum-keyed API and
adds a bulk read/write path sized for the FFI bridge. The bridge needs to
hand a full guest-state snapshot to native code (and accept one back) in a
single call. Today doing that means N round-trips through individual
accessors, with no compile-time check that the codes are wired into the right
accessor at the call site.

Three load-bearing constraints from existing artifacts shape the design:

- `include/gaby_vm/` must not include any VIXL header or reference any
  `vixl::*` symbol (from the `aarch64-simulator` spec's "Public header surface
  does not expose VIXL types" requirement).
- The existing `WritePc` re-entrancy contract — "do not use it to seat a
  nested step from inside a leaf" — is non-obvious and must survive the move
  to the typed surface.
- The public surface is Pimpl-wrapped; new entry points cross the Pimpl
  boundary the same way existing ones do.

## Goals / Non-Goals

**Goals**

- Type-safe register identification at compile time: an X-register code can
  never be passed where a V-register code is expected, and vice versa.
- One-call snapshot and restore of the full guest architectural state, for
  the FFI bridge.
- A partial batch-write entry point that does not force a full-state read
  first.
- ABI-stable POD for the types that genuinely cross the FFI boundary
  (`RegisterFile`, the standalone enums, `VRegisterValue`). C / Swift / other
  FFI consumers must be able to read and write them without a binding-side
  wrapper layer.
- No `vixl::*` symbol or imported header leaks into the new public header.

Notes:

- ABI stability does **not** extend to the `std::span` partial-write surface
  (see Decision 5). That path is a C++ ergonomic convenience; FFI consumers
  drive partial writes through individual typed `Write` calls instead, or
  through `WriteAll(RegisterFile)` if the whole snapshot is on hand.

**Non-Goals**

- A full C-ABI binding layer for the simulator. This design only commits to
  ABI-stable types in C++ public headers; binding generators are a separate
  concern.
- Changing register or simulator semantics. SP-vs-XZR encoding, V-register
  width, flag layout, BTI tracking, and re-entrancy guarantees are all
  preserved.
- W-register (32-bit) accessors on the public surface. The public API has
  only X today; extending to W is a follow-up if a real caller appears.
- Atomicity / rollback for the batch write. A failing entry aborts; entries
  that ran before it stay applied (matches the sequence-of-individual-writes
  mental model).
- Validation of system-register *content* (e.g. legal NZCV bit patterns).
  The write surface is unchecked, mirroring the existing X-register write
  posture: caller's responsibility.

## Decisions

### Decision 1: One `GpRegister` enum covering X / SP / PC, plus `LR` alias

```cpp
enum class GpRegister : uint8_t {
  X0  =  0, X1  =  1, /* ... */ X30 = 30,
  LR  = 30,                          // alias for X30
  SP  = 31,
  PC  = 32,
};
```

Rationale: collapsing all 64-bit register slots into a single enum keeps the
call-site form uniform — `sim.Write(GpRegister::X3, v)`, `sim.Write(GpRegister::SP, v)`,
`sim.Write(GpRegister::PC, pc)`. The underlying values are an internal
encoding choice that the implementation uses to dispatch; the API does not
promise they match VIXL's internal codes.

`enum class : uint8_t` fixes the underlying width at one byte, removes
implicit conversions, and makes the type safe to expose across an FFI
boundary.

**Alternatives considered.** Three separate single-value enums for SP, PC,
and PC-vs-SP wrappers. Rejected: ceremonial, makes overload sets noisy
(one `Write` per enum), and forces FFI consumers to learn three types where
one suffices.

### Decision 2: Two more enums — `VRegister` and `SysRegister`

```cpp
enum class VRegister  : uint8_t { V0 = 0, V1 = 1, /* ... */ V31 = 31 };
enum class SysRegister: uint8_t { NZCV = 0, FPCR = 1, FPSR = 2, BType = 3 };
```

`SysRegister` covers exactly the four sysregs the existing public surface
already touches. Underlying values are an internal-dispatch detail.

Why a separate `SysRegister` instead of folding sysregs into `GpRegister`:
the value widths differ (sysregs are `uint32_t`, GP regs are `uint64_t`).
Separating them lets the typed `Read` / `Write` overloads have correctly
typed return / parameter values, instead of widening sysregs to `uint64_t`
and losing the type signal.

### Decision 3: Header placement — new `include/gaby_vm/registers.h`

The three enums plus `VRegisterValue`, `RegisterFile`, and `RegisterWrite`
live in `include/gaby_vm/registers.h`. `simulator.h` includes `registers.h`
for the accessor signatures.

Why a separate header:

- `simulator.h` is already a thick public surface; the register definitions
  are independently useful (an FFI consumer constructing a `RegisterFile`
  doesn't need the `Simulator` class declaration in scope).
- A change to register identifiers should not force a recompile of
  everything that depends on `simulator.h`.
- `VRegisterValue` currently lives in `simulator.h`. It moves to
  `registers.h`; `simulator.h` keeps the type visible through the include,
  so existing call sites (e.g. `ShadowRunner`) continue to compile against
  the same fully-qualified name `gaby_vm::VRegisterValue`.

### Decision 4: `RegisterFile` POD layout

```cpp
struct RegisterFile {
  uint64_t        x[31];   // X0..X30
  uint64_t        sp;
  uint64_t        pc;      // host address; widened from uintptr_t
  VRegisterValue  v[32];   // V0..V31, each 128 bits
  uint32_t        nzcv;
  uint32_t        fpcr;
  uint32_t        fpsr;
  uint32_t        btype;
};
```

- All fields are fixed-width integers. No `uintptr_t`, no `size_t`, no
  `bool`. This makes the layout host-pointer-width-independent.
- Field order is "most architectural" → "least": GPRs, then SP/PC, then V,
  then sysregs. V comes after SP/PC so a caller that wants just the integer
  state can blit the prefix.
- `pc` widens from the public `uintptr_t` (in `WritePc`) to `uint64_t`. The
  conversion is a no-op on the targeted 64-bit hosts (iOS arm64, macOS
  arm64, Linux/Android/HarmonyOS on aarch64 or x86_64); we already commit to
  64-bit-only targets elsewhere in the project. The typed
  `Write(GpRegister::PC, uintptr_t)` accessor still takes `uintptr_t` to
  keep call-site ergonomics for `void*`-style PCs.
- Natural alignment of the struct is 8 bytes; `VRegisterValue` (two
  `uint64_t`) is 8-byte aligned, so the array slot does not introduce
  padding. The `uint32_t` tail fits the natural alignment with no implicit
  trailing pad.

A `static_assert(sizeof(RegisterFile) == 31*8 + 8 + 8 + 32*16 + 16)` will
freeze the layout at the point of definition.

**Alternatives considered.**

- `std::array<uint64_t, 31>` for the GP slots instead of a raw `uint64_t[31]`.
  Rejected: `std::array` is layout-compatible with the raw array, but using
  it forces `<array>` into the header for no functional gain.
- A nested `struct GpRegs { ... }` etc. Rejected: nesting buys structure but
  costs FFI ergonomics (consumers nest field paths). The flat layout is
  trivially mirrorable in C.

### Decision 5: `RegisterWrite` is a `std::variant` over typed write structs

```cpp
struct GpWrite  { GpRegister  reg; uint64_t       value; };
struct VWrite   { VRegister   reg; VRegisterValue value; };
struct SysWrite { SysRegister reg; uint32_t       value; };

using RegisterWrite = std::variant<GpWrite, VWrite, SysWrite>;

void Simulator::Write(std::span<const RegisterWrite> writes);
```

Construction is type-safe by the variant's converting constructor:
`RegisterWrite{GpWrite{GpRegister::X3, 42}}`. The dispatch loop inside the
Pimpl uses `std::visit` with an `overloaded` lambda set, so each branch sees
the right enum + value pair without manual `uint8_t` casts. There is no
out-of-band `kind` discriminator on the public surface; the variant carries
its own.

Why this is the right shape after weighing FFI stability:

- The primary FFI use case the proposal calls out — full-state context
  handoff — is served by `RegisterFile` + `ReadAll` / `WriteAll`. That path
  stays a flat POD (Decision 4) and remains directly consumable from C /
  Swift.
- The span-based partial write is a C++ ergonomic convenience for "set up
  N specific registers before a call." A FFI consumer that wants partial
  writes calls individual `sim.Write(GpRegister::X0, …)` entries instead;
  the per-call cost across the Pimpl boundary is small.
- Trading FFI-stability of this one type for genuine type-system safety is
  a good deal: a foreign consumer hand-stamping a "kind = kV but reg slot
  holding a GP code" mismatched POD is exactly the class of bug variant
  forecloses.

**Alternatives considered.**

- **Hand-rolled tagged POD** (the previous draft): explicit `Kind` enum,
  raw `uint8_t reg` field, 16-byte payload union, factory `Make` helpers.
  ABI-stable but the wire form has representable "kind/reg mismatch"
  states, plus the dispatch implementation needs hand-written switching
  and post-cast validation. Rejected once we realised `RegisterFile`
  alone already covers the FFI bridge's primary need.
- **Three separate spans** (`Write(span<GpWrite>, span<VWrite>,
  span<SysWrite>)`). Rejected: forces callers to bucket their writes by
  kind and pre-allocate three spans even when most writes are GP.
- **An untagged union plus a parallel `span<Kind>`**. Rejected: two-span
  APIs are easy to call out-of-step.

### Decision 6: Span-write error model — fail-fast, no rollback

`Simulator::Write(std::span<const RegisterWrite>)` applies writes in order.
The variant rules out kind/reg mismatch at the type level, so the only
remaining invalid case is an out-of-range enum value (e.g. a `GpRegister`
constructed by `static_cast`-ing a numeric `99`). On encountering such an
entry the implementation aborts with a diagnostic. Entries that ran before
the bad one are applied; entries after it are not. There is no
transactional rollback.

Rationale:

- Matches the rest of the `gaby_vm::Simulator` surface: misuse aborts
  rather than returning a status (e.g. the cache track aborts on a PC that
  leaves every registered code range; the constructor aborts on a null
  cache when the cache track is later used).
- Atomicity would force the implementation to snapshot the affected slots,
  then commit, which doubles the work on every span write and forces an
  allocation. No caller has a use for partial-failure semantics.
- For in-tree call sites, the variant's typed constructors make malformed
  entries actively hard to write; abort exists as a defensive backstop for
  the `static_cast` escape hatch.

### Decision 7: SP / PC routing through the typed Write

`Simulator::Write(GpRegister r, uint64_t value)` dispatches inside the
Pimpl on `r`:

- `X0..X30` → vixl `Simulator::WriteXRegister(code, value)`.
- `SP`      → vixl `Simulator::WriteSp(value)` (the SP slot, not XZR).
- `PC`      → vixl `Simulator::WritePc(value)`.

Same fan-out for the `Read` overload. This collapses the previously bespoke
`WriteSp` / `WritePc` / `WriteXRegister` into one typed call form.

**Re-entrancy contract for `GpRegister::PC` writes survives the move.** The
existing `WritePc` docstring spells out that the call must NOT be used to
seat a nested step from inside a leaf, because it mutates the PC before the
enclosing run's cursor is snapshotted. That property is intrinsic to the
PC slot — it doesn't depend on the surface form — and the typed
`Write(GpRegister::PC, ...)` inherits it. The docstring moves to the typed
accessor verbatim, and the existing `RunFrom` / `StepOnce(entry_pc)` /
`DebugStepOnce(entry_pc)` re-entrancy escape hatches stay.

`WriteAll(const RegisterFile&)` does write the PC slot, but only as part of
a top-level state-restore call: it does not nest from inside a leaf. The
docstring on `WriteAll` reiterates that the call is for top-level use only,
matching the same constraint.

### Decision 8: Sysreg writes — Pimpl mechanics

VIXL's internal `Simulator` already writes NZCV, FPCR, and FPSR as part of
ordinary instruction semantics; the Pimpl gains thin pass-through methods
that surface those internal writes through `gaby_vm::Simulator::Write(SysRegister,
uint32_t)`.

BType is exposed through the typed write surface, mirroring NZCV / FPCR /
FPSR. Existing BTI tracking semantics aren't surfaced (or relied on) at
the public boundary today, so the typed write is just a slot-update. If a
future change wires BTI invariants through the public surface, the write
path can grow validation at that point.

### Decision 9: Pimpl boundary — no marker comments in imported code

The new entry points are added on `gaby_vm::Simulator` (project-authored,
unmarked code). The Pimpl `Impl` translates each typed call into the
appropriate internal `vixl::aarch64::Simulator` operation; these internal
operations already exist in the imported VIXL surface (used by instruction
semantics) and require no changes to imported files. No `gaby-vm` marker
comments are added by this change.

## Risks / Trade-offs

- **Risk:** Removing the bespoke `WriteSp` / `WritePc` accessors loses the
  in-header docstring placement for the SP/PC-specific re-entrancy contract.
  → **Mitigation:** Move the `WritePc` docstring verbatim onto
  `Write(GpRegister, uint64_t)` with explicit note that the PC case is the
  one carrying the re-entrancy hazard. Add a test that exercises the
  typed-PC path inside a nested-step scenario to lock the contract.

- **Risk:** `RegisterFile` layout drift on future host targets. If a 32-bit
  host is ever added, fields stay correct (all `uint64_t` / `uint32_t`) but
  alignment may shift in subtle ways with embedded compilers.
  → **Mitigation:** `static_assert(sizeof(RegisterFile) == …)` in
  `registers.h` freezes layout at compile time; any drift becomes a build
  error. The project commits to 64-bit hosts; if that ever changes the
  assertion forces a deliberate re-check.

- **Risk:** Construction of an enum value via `static_cast` from a numeric
  literal can still produce an out-of-range register identifier (e.g.
  `static_cast<GpRegister>(99)`). The variant prevents kind/value mismatch
  but does not constrain the enum's underlying value.
  → **Mitigation:** Each Pimpl write path range-checks the enum after the
  variant has dispatched; out-of-range aborts. Document the validation
  rule in the spec-delta requirements.

- **Trade-off:** `RegisterWrite` (the `std::variant`) is not ABI-stable
  across libc++/libstdc++ versions — `std::variant`'s internal storage
  layout is implementation-defined. The span partial-write path is
  therefore C++-only.
  → **Mitigation accepted:** The FFI bridge's primary need is full-state
  context handoff, served by the POD `RegisterFile` (Decision 4). FFI
  consumers reach partial writes through individual typed `Write(...)`
  calls; the per-call cost is small. The variant's type-safety win
  (no representable kind/reg mismatch) is judged worth the loss of FFI
  shareability for this one type.

- **Risk:** Internal migration churn (tests, `ShadowRunner`, benchmark
  harness) is large.
  → **Mitigation:** The migration is mechanical (one-to-one rewrite,
  arity-preserving). Enumerated as concrete sub-tasks in `tasks.md`. The
  build does not compile until migration is complete — that is the
  enforcement.

- **Trade-off:** Exposing NZCV / FPCR / FPSR / BType writes lets a caller
  put the simulator into states it would otherwise only reach through
  instruction execution. We accept this — the existing X-register write
  surface already has the same property — and document the caller-owns-it
  posture explicitly.

- **Trade-off:** `WriteAll` is one Pimpl call but internally is a sequence
  of individual writes. No O(1) memcpy-style fast path. Acceptable: the
  surface savings are at the call site (one call instead of ~70), not in
  the per-call overhead.

## Migration Plan

Phased, within this change:

1. Create `include/gaby_vm/registers.h` with `GpRegister`, `VRegister`,
   `SysRegister`, `VRegisterValue` (moved from `simulator.h`),
   `RegisterFile`, the typed write structs `GpWrite` / `VWrite` /
   `SysWrite`, and the `RegisterWrite = std::variant<…>` alias.
2. Update `include/gaby_vm/simulator.h`: include `registers.h`; add typed
   `Read` / `Write` overloads, `ReadAll` / `WriteAll`,
   `Write(std::span<const RegisterWrite>)`; remove old `unsigned`-coded
   accessors and bespoke `WriteSp` / `WritePc` accessors.
3. Update `Simulator::Impl` to route typed calls and expose sysreg writes.
4. Migrate internal callers (`tests/`, `shadow_runner`,
   `benchmark-harness`) to the typed surface. The build does not compile
   until this completes.
5. Verify `simulator_smoke` and `simulator_correctness` (and any other
   registered CTest cases) pass under both `dev-debug` and `dev-release`.

No rollback strategy is needed: changes are confined to one branch; if
issues surface, revert is by `git revert`.

## Open Questions

- **W-register (32-bit) accessors on the public surface.** Not added by
  this change. If a real caller appears, it'll come in a follow-up
  proposal; for now `Read(GpRegister::X0) & 0xffffffff` is the workaround.
- **C-binding header.** This change commits to ABI-stable C++ for the FFI
  primitives (`RegisterFile`, the standalone enums, `VRegisterValue`) but
  does not ship a `.h`-only mirror for C consumers. The POD types are
  hand-mirrorable; a generated mirror is out of scope. The
  `std::variant`-based span path is intentionally C++-only — see the
  Risks/Trade-offs section.
- **Future writable `RegisterFile`-prefix snapshot.** The current design
  uses one `WriteAll`. If embedders want "write GP-only" or "write V-only"
  fast paths, that would justify additional bulk overloads. Defer until
  a concrete embedder asks.
