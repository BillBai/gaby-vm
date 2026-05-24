## Why

The `Simulator` public surface today only exposes register state through
per-register accessors keyed by untyped `unsigned` codes
(`WriteXRegister(unsigned, uint64_t)`, `ReadVRegister(unsigned)`, …). The
simulator ⇄ embedder FFI bridge needs to move whole register snapshots between
guest and native state in one call — for context handoff, callback marshaling,
and oracle comparisons — and doing that through N per-register pings is both
verbose and slow. The `unsigned`-coded form also gives no type-level protection:
nothing stops a V-register code being passed to an X-register accessor, and
SP / PC each need their own bespoke accessors today because the encoding can't
be expressed in the parameter type.

This change replaces that surface with a strongly-typed, enum-keyed register
I/O API and adds bulk read / write entry points sized for the FFI bridge.

## What Changes

- **Add strongly-typed register identifiers.** New header
  `include/gaby_vm/registers.h` exposes:
  - `enum class GpRegister : uint8_t` covering `X0`..`X30`, an `LR = X30`
    alias, `SP`, and `PC`. Underlying width is fixed at `uint8_t` so the
    enum is ABI-stable and safe to use across the FFI boundary.
  - `enum class VRegister : uint8_t` covering `V0`..`V31`.
  - `enum class SysRegister : uint8_t` covering `NZCV`, `FPCR`, `FPSR`, `BType`
    — exposed even where it's currently read-only, so the bulk-write surface
    can address them by a single tag.
  The header contains no `vixl::*` symbols and no imported-VIXL includes,
  preserving the current public-API boundary.
- **Add enum-keyed `Read` / `Write` overloads on `Simulator`.** Call sites
  become `sim.Write(GpRegister::X0, val)`, `sim.Read(VRegister::V3)`,
  `sim.Read(SysRegister::NZCV)`. The `GpRegister` overload internally routes
  `SP` and `PC` to the right backing slot, retiring the need for separate
  `WriteSp` / `WritePc` call sites in new code.
- **Add a POD `RegisterFile` snapshot type and `ReadAll` / `WriteAll`.**
  `RegisterFile` carries the full guest architectural state — `X0`..`X30`, SP,
  PC, `V0`..`V31` as `VRegisterValue` pairs, plus NZCV / FPCR / FPSR / BType.
  Layout is fixed (no implementation-defined padding) so the struct is
  shareable across the FFI boundary. `Simulator::ReadAll() -> RegisterFile`
  snapshots, `Simulator::WriteAll(const RegisterFile&)` restores. This is the
  primary path the FFI bridge uses for context handoff.
- **Add `Simulator::Write(std::span<const RegisterWrite>)` for partial batch
  writes.** `RegisterWrite` is a tagged POD: a discriminator byte plus a
  payload union sized to hold either a 64-bit GP / sys value or a 128-bit V
  value. This serves the "set up N specific registers before a call" path,
  where `WriteAll` would force the caller to read the full state first.
- **Add write accessors for `NZCV` / `FPCR` / `FPSR` / `BType`.** They are
  read-only today; making them writable through the typed surface is what
  lets `WriteAll` and the span-based batch write be symmetric with the read
  side. New per-register typed setters on `Simulator` (`sim.Write(SysRegister::NZCV, …)`)
  back this.
- **BREAKING: remove the existing `unsigned`-keyed accessors.**
  `WriteXRegister(unsigned, ...)` / `ReadXRegister` / `WriteSp` / `ReadSp` /
  `WritePc` / `ReadPc` / `ReadVRegister(unsigned)` are deleted from the public
  surface; the typed enum-keyed overloads are the only way to read or write
  register state after this change. Internal callers (tests, `ShadowRunner`,
  benchmark harness) are migrated in the same change so the tree never builds
  in a half-migrated state. The project is still pre-1.0 with no committed
  external API contract, so a one-shot break is cheaper than carrying a
  deprecated parallel surface — and it forecloses the foot-gun the change is
  meant to eliminate (an `unsigned` V-register code reaching an X-register
  accessor) instead of leaving it opt-in.
- **No semantic change.** Register backing storage, SP-vs-XZR distinction,
  V-register width, and re-entrancy rules around `WritePc` are unchanged.
  This is purely an API surface change.

## Capabilities

### New Capabilities
- _(none)_

### Modified Capabilities
- `aarch64-simulator`: register I/O surface gains typed enum identifiers,
  enum-keyed `Read` / `Write` overloads, a `RegisterFile` snapshot POD with
  `ReadAll` / `WriteAll`, a `std::span<RegisterWrite>`-based partial batch
  write, and writable accessors for NZCV / FPCR / FPSR / BType. **BREAKING:**
  the previous `unsigned`-coded accessors (`WriteXRegister(unsigned, …)`,
  `ReadXRegister`, `WriteSp`, `ReadSp`, `WritePc`, `ReadPc`,
  `ReadVRegister(unsigned)`) are removed.

## Impact

- **Public headers.** New file `include/gaby_vm/registers.h`. New entry points
  on `gaby_vm::Simulator` in `include/gaby_vm/simulator.h`. The Pimpl shape and
  the "no `vixl::*` types leak" property are preserved.
- **Internal call sites.** `tests/`, `shadow_runner` implementation, and the
  `benchmark-harness` runners currently use the `unsigned`-coded accessors.
  They are migrated to the new typed surface inside this change as a hard
  requirement — the build will not compile until every caller is converted,
  since the old entry points are gone.
- **ABI.** All new enums are `enum class : uint8_t`; `RegisterFile` and
  `RegisterWrite` are designed as C-layout POD with explicitly ordered fields
  and no implementation-defined padding, so the bulk surface is usable from
  C / Swift / other FFI consumers without binding-side glue.
- **No behavior change.** No change to register semantics, no change to either
  execution track, no change to the memory-write observer, no change to
  re-entrancy guarantees. The two-track equivalence checked by `ShadowRunner`
  is unaffected.
- **Open design questions** (resolved in `design.md`, not here):
  - Exact field order and packing of `RegisterFile` and `RegisterWrite`.
  - Whether `GpRegister::SP` and `GpRegister::PC` get dedicated underlying
    values (e.g. `31`, `32`) or sit in a separate small enum to keep the
    GP-only path tighter — trade-off between API surface count and call-site
    ergonomics.
  - Whether the span-based batch write reports per-element errors or
    fail-fast on the first invalid entry.
