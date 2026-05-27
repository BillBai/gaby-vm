## ADDED Requirements

### Requirement: NEON ClearForWrite SHALL use a bulk-clear path with a single dirty-flag notification

`LogicVRegister::ClearForWrite(VectorFormat)` SHALL clear the tail bytes of the underlying `SimRegisterBase` storage (those beyond the destination format's logical size) with a single bulk operation, and SHALL invoke `NotifyRegisterWrite()` exactly once per call regardless of how many bytes were cleared. The implementation MUST NOT dispatch per-byte through `SetUint` or any other lane-write helper.

The bulk-clear MUST be reachable through a helper method on
`SimRegisterBase` (named `ClearTail` or equivalent narrow-privilege
accessor) so that `LogicVRegister` does not need direct mutable access
to the register's byte storage. The helper's only behavior is to zero
the requested tail byte range and call `NotifyRegisterWrite()` once.

The implementation lives inside the existing imported VIXL header
(`src/aarch64/simulator-aarch64.h`) and MUST be enclosed in a gaby-vm
marker block (`// gaby-vm BEGIN:` … `// gaby-vm END`), per the marker
convention required by the "Imported files are byte-identical to
upstream except at marked locations" requirement. The marker block's
reason text SHALL identify this change and point back to the
profile-driven motivation (`docs/refs/gaby-vm-cache-hotpath-profile-2026-05-27.md`).

The observable simulator state after `ClearForWrite` MUST be identical
to the upstream byte-loop implementation: the destination's tail bytes
read as zero, the `written_since_last_log_` flag reads `true`, and no
other observable state differs. The semantic equivalence rests on the
fact that `written_since_last_log_` is a boolean — setting it to `true`
once and setting it to `true` N times are identical operations.

#### Scenario: Tail bytes are cleared in a single bulk operation

- **WHEN** `ClearForWrite(kFormat8B)` (or any half-width destination
  format) is invoked on a `LogicVRegister` wrapping a fresh
  `SimVRegister`
- **THEN** the implementation invokes `memset` (or equivalent bulk
  zero-fill) on the tail byte range exactly once
- **AND** the implementation does NOT invoke `SetUint` for any byte
  in the tail

#### Scenario: NotifyRegisterWrite fires exactly once per ClearForWrite call

- **WHEN** `ClearForWrite(vform)` is invoked for any non-SVE `vform`
  that does not fully cover the register
- **THEN** `NotifyRegisterWrite()` is called exactly one time inside
  that invocation
- **AND** the `written_since_last_log_` flag is observable as `true`
  afterward

#### Scenario: Full-width formats incur no work beyond format checks

- **WHEN** `ClearForWrite(kFormat16B)` is invoked (destination size
  equals register size)
- **THEN** no `memset` is invoked and no `NotifyRegisterWrite` is
  called inside `ClearForWrite`
- **AND** the function returns with the underlying register state
  unchanged

#### Scenario: SVE formats early-return as before

- **WHEN** `ClearForWrite(kFormatVnB)` (or any other SVE format) is
  invoked
- **THEN** the function returns immediately without touching any byte
  storage and without invoking `NotifyRegisterWrite`

#### Scenario: workload_shadow reports zero divergence

- **WHEN** the `workload_shadow` test runs every committed bench
  workload against both the decoder and cache execution tracks
- **THEN** the oracle reports zero divergence across all workloads
  and all observable simulator state (general-purpose registers,
  vector registers, flags, memory)

### Requirement: Remaining VectorFormat helpers SHALL be defined to allow compile-time constant folding

The eight imported VIXL `VectorFormat` helpers — `vixl::aarch64::ScalarFormatFromLaneSize`, `LaneSizeInBytesLog2FromFormat`, `MaxLaneCountFromFormat`, `IsVectorFormat`, `ScalarFormatFromFormat`, `MaxIntFromFormat`, `MinIntFromFormat`, `MaxUintFromFormat` — SHALL be defined as `constexpr inline` functions in `src/aarch64/instructions-aarch64.h`, so the compiler MUST be able to constant-fold them at call sites where the input is a compile-time constant.

Additionally, the helper `vixl::GetUintMask` in `src/utils-vixl.h`,
which is called by `MaxIntFromFormat` and `MaxUintFromFormat`, SHALL
be promoted from `inline` to `constexpr inline` so that the callers'
constexpr eligibility is not broken by the dependency.

These helpers MUST remain inside gaby-vm marker blocks
(`// gaby-vm BEGIN:` … `// gaby-vm END`) per the marker convention
already required by the "Imported files are byte-identical to upstream
except at marked locations" requirement. Each marker block's reason
text SHALL identify this change and point back to the current source
location in upstream.

The corresponding source file (`src/aarch64/instructions-aarch64.cc`)
SHALL retain marker blocks at the original definition sites, noting
that the definitions have been lifted to the header. The marker blocks
in the source MUST NOT contain stale function-body copies (which would
produce ODR conflicts with the header inline definitions).

The switch bodies — case labels, return values, internal calls to
other helpers in the same family, and the use of `VIXL_ASSERT` /
`VIXL_UNREACHABLE` — MUST be byte-equivalent to the upstream
definitions. Promotion is a code-organization optimization, not a
semantic change.

#### Scenario: Header carries inline definitions inside marker blocks

- **WHEN** the file `src/aarch64/instructions-aarch64.h` is inspected
  for the definitions of the eight listed helpers
- **THEN** each helper is defined as a `constexpr inline` function in
  the header
- **AND** the definitions are bracketed by `// gaby-vm BEGIN:` /
  `// gaby-vm END` blocks whose comment text explains why the
  definitions were promoted and references this change

#### Scenario: utils-vixl.h GetUintMask is constexpr inline

- **WHEN** the file `src/utils-vixl.h` is inspected for the definition
  of `vixl::GetUintMask`
- **THEN** the function is defined as `constexpr inline` (not plain
  `inline`)
- **AND** the change is enclosed in a `// gaby-vm BEGIN:` /
  `// gaby-vm END` block whose comment text references this change

#### Scenario: Source file does not redefine the helpers

- **WHEN** the file `src/aarch64/instructions-aarch64.cc` is inspected
  for the bodies of the eight listed helpers
- **THEN** no helper has a function body in the source file
- **AND** each position formerly occupied by a body contains a
  `// gaby-vm BEGIN:` / `// gaby-vm END` marker block whose comment
  text records that the definition has been lifted to the header

#### Scenario: Compile-time constant folding works for the new helpers

- **WHEN** a `static_assert` is written in any compilation unit that
  includes `instructions-aarch64.h` — for instance,
  `static_assert(::vixl::aarch64::MaxIntFromFormat(::vixl::aarch64::kFormat4S) == INT32_MAX)`
  or
  `static_assert(::vixl::aarch64::ScalarFormatFromFormat(::vixl::aarch64::kFormat4S) == ::vixl::aarch64::kFormatS)`
- **THEN** the translation unit compiles successfully without emitting
  a runtime call to the helper

#### Scenario: Switch bodies match upstream byte-for-byte

- **WHEN** the case labels, return values, and assertions of each of
  the eight listed helpers in `instructions-aarch64.h` are compared
  with the upstream definitions in
  `../vixl/src/aarch64/instructions-aarch64.cc`
- **THEN** the case labels and returned values are identical, the
  `VIXL_ASSERT` and `VIXL_UNREACHABLE` placements are identical, and
  the only differences are the `constexpr inline` linkage qualifiers
  and the file location
