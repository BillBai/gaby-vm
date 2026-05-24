## ADDED Requirements

### Requirement: Public register-identifier header is provided

The repository SHALL contain a public header `include/gaby_vm/registers.h`
declaring the strongly-typed register identifiers and FFI-stable register-data
types used by the `gaby_vm::Simulator` typed register-I/O surface. In the
`gaby_vm` namespace the header SHALL declare:

- `enum class GpRegister : uint8_t`, with enumerators `X0`..`X30` taking
  underlying values 0..30, an `LR` enumerator that is an alias for `X30`,
  an `SP` enumerator with underlying value 31, and a `PC` enumerator with
  underlying value 32.
- `enum class VRegister : uint8_t`, with enumerators `V0`..`V31` taking
  underlying values 0..31.
- `enum class SysRegister : uint8_t`, with enumerators `NZCV`, `FPCR`,
  `FPSR`, and `BType`.
- `struct VRegisterValue { uint64_t lo; uint64_t hi; }`, moved from
  `simulator.h` so the typed surface and `RegisterFile` can refer to it
  through `registers.h`.
- `struct RegisterFile` (see the "RegisterFile is a POD with frozen layout"
  requirement).
- `struct GpWrite { GpRegister reg; uint64_t value; }`,
  `struct VWrite { VRegister reg; VRegisterValue value; }`,
  `struct SysWrite { SysRegister reg; uint32_t value; }`, and
  `using RegisterWrite = std::variant<GpWrite, VWrite, SysWrite>`.

The header SHALL NOT include any imported VIXL header and SHALL NOT reference
any `vixl::*` symbol. It SHALL be self-contained: a translation unit that
`#include`s only `<gaby_vm/registers.h>` (and no other gaby-vm header) SHALL
compile.

#### Scenario: Header exists at the public-API path

- **WHEN** the path `include/gaby_vm/registers.h` is checked
- **THEN** the file exists

#### Scenario: Enums declare a `uint8_t` underlying type

- **WHEN** `include/gaby_vm/registers.h` is parsed for the declarations of
  `GpRegister`, `VRegister`, and `SysRegister`
- **THEN** each declaration has the form `enum class <name> : uint8_t`

#### Scenario: `GpRegister` covers X0..X30, LR, SP, PC with the documented codes

- **WHEN** the enumerator values of `gaby_vm::GpRegister` are inspected
- **THEN** `X0` through `X30` have underlying values 0 through 30
  respectively, `LR` has the same underlying value as `X30`, `SP` has
  underlying value 31, and `PC` has underlying value 32

#### Scenario: No VIXL include or symbol leaks into `registers.h`

- **WHEN** `git grep -nE 'vixl|aarch64/' include/gaby_vm/registers.h` is run
- **THEN** the output is empty

#### Scenario: Header is self-contained

- **WHEN** a translation unit `#include`s only `<gaby_vm/registers.h>` and
  no other gaby-vm header
- **THEN** the translation unit compiles without error

### Requirement: Simulator exposes typed register Read/Write accessors

`gaby_vm::Simulator` SHALL expose the following member functions in
`include/gaby_vm/simulator.h`. The Read overloads SHALL be `const`-qualified:

- `void Write(GpRegister reg, uint64_t value);` and
  `uint64_t Read(GpRegister reg) const;`
- `void Write(VRegister reg, VRegisterValue value);` and
  `VRegisterValue Read(VRegister reg) const;`
- `void Write(SysRegister reg, uint32_t value);` and
  `uint32_t Read(SysRegister reg) const;`

Each typed `Write` SHALL update the same backing register slot that the
corresponding instruction-level semantic write reaches; each typed `Read`
SHALL return the current value of that slot. A `Write` immediately followed
by a `Read` of the same register identifier on the same `Simulator` (with no
instruction executed in between) SHALL return the value just written.

The `GpRegister::PC` case of `Write(GpRegister, uint64_t)` SHALL retain the
re-entrancy contract previously attached to `WritePc`: it MAY be used to seat
a top-level entry point before a `StepOnce()` / `DebugStepOnce()` loop, but
MUST NOT be used to seat a *nested* step from inside a leaf executed by an
enclosing run. Re-entrant callers SHALL seat a nested entry point through
`RunFrom`, `StepOnce(entry_pc)`, or `DebugStepOnce(entry_pc)`. The docstring
on `Write(GpRegister, uint64_t)` SHALL state this constraint explicitly for
the PC case and SHALL name the three re-entrant alternatives.

#### Scenario: GP register write round-trips through Read

- **WHEN** a caller invokes `sim.Write(GpRegister::X3, 0xdeadbeefdeadbeef)`
  and then `sim.Read(GpRegister::X3)` without executing any instruction in
  between
- **THEN** the `Read` returns `0xdeadbeefdeadbeef`

#### Scenario: SP write reaches the SP slot, not XZR

- **WHEN** a caller invokes `sim.Write(GpRegister::SP, 0x1234567890abcdef)`
  and then `sim.Read(GpRegister::SP)`
- **THEN** the `Read` returns `0x1234567890abcdef`
- **AND** the value is observable as the simulator's stack pointer (not as
  the XZR-encoded slot 31)

#### Scenario: V register write preserves both 64-bit halves

- **WHEN** a caller invokes
  `sim.Write(VRegister::V5, VRegisterValue{0x1111111122222222, 0x3333333344444444})`
  and then `sim.Read(VRegister::V5)`
- **THEN** the `Read` returns
  `VRegisterValue{lo = 0x1111111122222222, hi = 0x3333333344444444}`

#### Scenario: System-register typed write round-trips

- **WHEN** a caller invokes `sim.Write(SysRegister::NZCV, 0x30000000)` and
  then `sim.Read(SysRegister::NZCV)`
- **THEN** the `Read` returns `0x30000000`
- **AND** equivalent round-trips for `FPCR`, `FPSR`, and `BType` also return
  the values just written

#### Scenario: PC write seats the cursor for a subsequent step

- **WHEN** a caller invokes `sim.Write(GpRegister::PC, entry_pc)` and then
  `sim.StepOnce()` (with no other state mutation in between)
- **THEN** the instruction at `entry_pc` is the one executed by `StepOnce`

#### Scenario: PC docstring documents the nested-step hazard

- **WHEN** the docstring attached to `Simulator::Write(GpRegister, uint64_t)`
  in `include/gaby_vm/simulator.h` is read
- **THEN** it states that the `GpRegister::PC` case MUST NOT be used to seat
  a nested step from inside a leaf, and names `RunFrom`,
  `StepOnce(entry_pc)`, and `DebugStepOnce(entry_pc)` as the re-entrant
  alternatives

### Requirement: Existing unsigned-coded register accessors are removed

`gaby_vm::Simulator`'s public surface in `include/gaby_vm/simulator.h` SHALL
NOT declare any of:

- `WriteXRegister(unsigned, uint64_t)` or `ReadXRegister(unsigned) const`,
- `WriteSp(uint64_t)` or `ReadSp() const`,
- `WritePc(uintptr_t)` or `ReadPc() const`,
- `ReadVRegister(unsigned) const`.

These entry points SHALL be wholly absent — they MAY NOT be retained as
deprecated forwarders. Internal call sites in the project (under `test/`,
`src/gaby_vm/`, and any in-tree benchmark target) SHALL be migrated to the
typed surface; the build SHALL NOT compile if any caller still names them.

#### Scenario: Removed accessor names are absent from the public header

- **WHEN** `git grep -nE '\b(WriteXRegister|ReadXRegister|WriteSp|ReadSp|WritePc|ReadPc|ReadVRegister)\b' include/gaby_vm/simulator.h` is run
- **THEN** the output is empty

#### Scenario: Project compiles with the removed accessors absent

- **WHEN** the project is configured and built (e.g.
  `cmake --preset dev-debug && cmake --build --preset dev-debug`)
- **THEN** the build succeeds end-to-end, demonstrating that no in-tree
  caller still references the removed entry points

### Requirement: `RegisterFile` is a POD with frozen layout

`include/gaby_vm/registers.h` SHALL declare `struct RegisterFile` with this
exact field order and exact field types:

| order | field   | type                  |
|-------|---------|-----------------------|
| 1     | `x`     | `uint64_t[31]`        |
| 2     | `sp`    | `uint64_t`            |
| 3     | `pc`    | `uint64_t`            |
| 4     | `v`     | `VRegisterValue[32]`  |
| 5     | `nzcv`  | `uint32_t`            |
| 6     | `fpcr`  | `uint32_t`            |
| 7     | `fpsr`  | `uint32_t`            |
| 8     | `btype` | `uint32_t`            |

`RegisterFile` SHALL be a standard-layout, trivially-copyable POD. A
`static_assert` in `registers.h` SHALL fix
`sizeof(RegisterFile) == 31*8 + 8 + 8 + 32*16 + 4*4`. The struct SHALL NOT
contain any `uintptr_t`, `size_t`, or `bool` field, so its layout is
independent of host pointer width.

#### Scenario: Field order matches the spec

- **WHEN** the declaration of `gaby_vm::RegisterFile` in
  `include/gaby_vm/registers.h` is inspected
- **THEN** the fields appear in the order `x`, `sp`, `pc`, `v`, `nzcv`,
  `fpcr`, `fpsr`, `btype`, with the types listed above

#### Scenario: `sizeof(RegisterFile)` is frozen at compile time

- **WHEN** `include/gaby_vm/registers.h` is searched for a `static_assert`
  on `sizeof(RegisterFile)`
- **THEN** the file contains a `static_assert` that asserts
  `sizeof(RegisterFile) == 31*8 + 8 + 8 + 32*16 + 4*4` (or an arithmetically
  equivalent literal such as `792`)

#### Scenario: `RegisterFile` is standard-layout and trivially copyable

- **WHEN** a translation unit evaluates
  `std::is_standard_layout_v<gaby_vm::RegisterFile> &&
   std::is_trivially_copyable_v<gaby_vm::RegisterFile>`
- **THEN** the expression is `true`

### Requirement: `ReadAll` / `WriteAll` snapshot and restore the full guest state

`gaby_vm::Simulator` SHALL expose:

- `RegisterFile ReadAll() const;`
- `void WriteAll(const RegisterFile& file);`

`ReadAll` SHALL populate every field of the returned `RegisterFile` from the
current value of the corresponding register slot. `WriteAll` SHALL update
every register slot from the corresponding field of `file`, with the same
observable effect as a sequence of individual typed `Write` calls that
covers each slot exactly once.

`WriteAll` is a top-level state-restore entry point. It SHALL NOT be called
from inside a leaf executed by an enclosing run; its docstring SHALL state
this constraint explicitly.

#### Scenario: ReadAll → WriteAll round-trips the architectural state

- **WHEN** a caller takes `auto snap = sim.ReadAll();`, executes any number
  of instructions, calls `sim.WriteAll(snap);`, and then takes
  `sim.ReadAll()` again
- **THEN** the second `ReadAll` result equals `snap` field-by-field

#### Scenario: WriteAll covers every architectural slot

- **WHEN** a caller constructs a `RegisterFile` with deliberately distinct
  values in every `x[i]`, `sp`, `pc`, `v[i]`, and sysreg field, invokes
  `sim.WriteAll(file)`, and then reads each slot back via the typed `Read`
  overloads
- **THEN** each typed `Read` returns the value the caller placed in the
  matching `RegisterFile` field

#### Scenario: WriteAll docstring documents the top-level-only constraint

- **WHEN** the docstring attached to `Simulator::WriteAll` in
  `include/gaby_vm/simulator.h` is read
- **THEN** it states that `WriteAll` is for top-level use only and MUST NOT
  be called from inside a leaf executed by an enclosing run

### Requirement: Batch write accepts a `std::span<const RegisterWrite>`

`gaby_vm::Simulator` SHALL expose a public member function
`void Write(std::span<const RegisterWrite> writes);` where
`RegisterWrite = std::variant<GpWrite, VWrite, SysWrite>` as declared in
`registers.h`.

Each element of `writes` SHALL be applied in span order. Applying an element
SHALL update the same backing slot, with the same value semantics, that the
matching single-element typed `Write` overload would update — selected by the
active variant alternative. The span write SHALL behave as the sequence of
typed `Write` calls that the variant alternatives encode: there is no
atomic-commit guarantee, and entries that ran before an aborting one stay
applied.

If an element carries an enum value whose underlying integer is outside the
declared range for that enum (for instance a `GpRegister` produced by
`static_cast<GpRegister>(99)`), the implementation SHALL abort with a
diagnostic that identifies the offending element's position in the span.

#### Scenario: Mixed GP / V / Sys batch is applied in order

- **WHEN** a caller invokes
  `sim.Write(std::array<RegisterWrite, 3>{
      GpWrite{GpRegister::X0, 1},
      VWrite{VRegister::V0, VRegisterValue{2, 3}},
      SysWrite{SysRegister::NZCV, 0x40000000},
   });`
- **THEN** `sim.Read(GpRegister::X0)` returns `1`,
  `sim.Read(VRegister::V0)` returns `VRegisterValue{2, 3}`, and
  `sim.Read(SysRegister::NZCV)` returns `0x40000000`

#### Scenario: Out-of-range enum value aborts the batch

- **WHEN** a caller invokes `sim.Write(...)` with a span containing an
  element whose `GpRegister` was constructed via
  `static_cast<GpRegister>(99)`
- **THEN** the call aborts with a diagnostic that identifies the offending
  element's position

#### Scenario: `RegisterWrite` resolves to the documented variant

- **WHEN** the declaration of `gaby_vm::RegisterWrite` in
  `include/gaby_vm/registers.h` is inspected
- **THEN** it is declared as
  `using RegisterWrite = std::variant<GpWrite, VWrite, SysWrite>;`
