## ADDED Requirements

### Requirement: NEON format helpers SHALL be defined to allow compile-time constant folding

The six imported VIXL helpers that map `VectorFormat` (a 6-bit enum) to lane size, lane count, register size, or format kind — `vixl::aarch64::IsSVEFormat`, `LaneSizeInBitsFromFormat`, `LaneSizeInBytesFromFormat`, `LaneCountFromFormat`, `RegisterSizeInBitsFromFormat`, `RegisterSizeInBytesFromFormat` — SHALL be defined as `constexpr inline` functions in `src/aarch64/instructions-aarch64.h`, so the compiler MUST be able to constant-fold them at call sites where the `VectorFormat` argument is a compile-time constant.

These helpers MUST remain inside a gaby-vm marker block in the header
(`// gaby-vm BEGIN:` … `// gaby-vm END`), per the marker convention
already required by the "Imported files are byte-identical to upstream
except at marked locations" requirement. The marker block's reason
text SHALL identify the change that promoted the definitions and
point back to the current source location in upstream
`instructions-aarch64.cc`, so a future re-import can find them.

The corresponding source file (`src/aarch64/instructions-aarch64.cc`)
SHALL retain a marker block at the original definition site, noting
that the definitions have been lifted to the header. The marker block
in the source MUST NOT contain a stale function-body copy (which would
produce an ODR conflict with the header inline definitions).

The switch body — case labels, return values, internal calls to other
helpers in the same family, and the use of `VIXL_ASSERT` /
`VIXL_UNREACHABLE` — MUST be byte-equivalent to the upstream
definition. Promotion is a code-organization optimization, not a
semantic change.

#### Scenario: Header carries inline definitions inside a marker block

- **WHEN** the file `src/aarch64/instructions-aarch64.h` is inspected
  for the definitions of the six listed helpers
- **THEN** each helper is defined as a `constexpr inline` function in
  the header
- **AND** the definitions are bracketed by a `// gaby-vm BEGIN:` /
  `// gaby-vm END` block whose comment text explains why the
  definitions were promoted and references this change

#### Scenario: Source file does not redefine the helpers

- **WHEN** the file `src/aarch64/instructions-aarch64.cc` is inspected
  for the bodies of the six listed helpers
- **THEN** no helper has a function body in the source file
- **AND** the position formerly occupied by the bodies contains a
  `// gaby-vm BEGIN:` / `// gaby-vm END` marker block whose comment
  text records that the definitions have been lifted to the header

#### Scenario: Compile-time constant folding works for literal VectorFormat

- **WHEN** a `static_assert` is written in any compilation unit that
  includes `instructions-aarch64.h` — for instance,
  `static_assert(::vixl::aarch64::LaneSizeInBitsFromFormat(::vixl::aarch64::kFormat4S) == 32)`
- **THEN** the translation unit compiles successfully without emitting
  a runtime call to the helper

#### Scenario: Switch bodies match upstream byte-for-byte

- **WHEN** the case labels, return values, and assertions of each of
  the six listed helpers in `instructions-aarch64.h` are compared with
  the upstream definitions in `../vixl/src/aarch64/instructions-aarch64.cc`
- **THEN** the case labels and returned values are identical, the
  `VIXL_ASSERT` and `VIXL_UNREACHABLE` placements are identical, and
  the only differences are the `constexpr inline` linkage qualifiers
  and the file location
