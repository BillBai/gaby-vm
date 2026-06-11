## MODIFIED Requirements

### Requirement: RegisterCodeRange validates inputs and reports failures all-or-nothing

`RegisterCodeRange` SHALL report its outcome through a
`PredecodeCache::RegistrationStatus` value. It SHALL return `OverlappingRange`
when the requested range overlaps any already-registered range in any way,
including an exact duplicate. It SHALL return `InvalidArgument` when the size is
zero or not a multiple of 4. It SHALL return `OutOfMemory` when entry storage
cannot be allocated. `RegisterCodeRange` SHALL reject a range only for these
structural reasons; no per-word decode property (an unallocated encoding, or an
encoding a stricter-than-default auditor would reject) SHALL cause rejection —
such a word is handled as embedded data (see "RegisterCodeRange tolerates
read-only data embedded in a code range"). On any non-`Ok` result the call SHALL
register nothing — no instruction from a failed call SHALL become executable
through the cache.

`RegisterCodeRange` SHALL NOT return `UnsupportedFeature`. The
`RegistrationStatus::UnsupportedFeature` enum value SHALL be retained for ABI
stability but SHALL NOT be produced: the cache's `CPUFeaturesAuditor` is
configured with `CPUFeatures::All()` (feature checking disabled), and a word
that needs a feature the auditor rejects is recorded as a data sentinel rather
than rejecting the range.

#### Scenario: Overlapping range is rejected

- **WHEN** a range that overlaps an already-registered range is passed to `RegisterCodeRange`
- **THEN** the call returns `OverlappingRange`
- **AND** the set of cache-executable instructions is unchanged

#### Scenario: Malformed size is rejected

- **WHEN** `RegisterCodeRange` is called with a size that is zero or not a multiple of 4
- **THEN** it returns `InvalidArgument` and registers nothing

#### Scenario: A failed registration is all-or-nothing

- **WHEN** a `RegisterCodeRange` call returns any non-`Ok` status
- **THEN** no instruction word from that call is executable through the cache track

#### Scenario: UnsupportedFeature is never returned

- **WHEN** a range contains a word whose encoding would require a CPU feature the auditor does not accept
- **THEN** `RegisterCodeRange` does not return `UnsupportedFeature`
- **AND** the range registers `Ok` with that word recorded as a non-executable data sentinel

## ADDED Requirements

### Requirement: RegisterCodeRange tolerates read-only data embedded in a code range

`RegisterCodeRange` SHALL register a code range successfully (`Ok`) even when the
range interleaves read-only data with instructions — inline literal pools
(`LDR x, =imm`), jump tables, and inline constants — so that some of its 4-byte
words do not decode to an executable cached instruction. A word that does not
decode to an executable cached instruction — whether its encoding is unallocated,
or (under any auditor stricter than the default `CPUFeatures::All()`) requires a
rejected CPU feature — SHALL be recorded as a non-executable **data sentinel**
and SHALL NOT cause the range to be rejected.

The cache SHALL NOT alter the bytes of a registered range. A load that reads a
data word SHALL therefore observe the original bytes through the simulator's
memory path, not the predecoded entry. A data-sentinel word SHALL NOT be
dispatched during normal execution (well-formed code branches over its data). If
cache-track execution reaches a data-sentinel word, the Simulator SHALL abort
with a diagnostic that names the offending address (trap-on-execute), the same
model used for unimplemented forms; it SHALL NOT execute the data as an
instruction, silently skip it, or fall back to the imported decoder.

#### Scenario: A range with embedded unallocated data registers Ok

- **WHEN** a range of real instructions that branch over an embedded data word whose encoding is unallocated (e.g. a literal-pool constant) is passed to `RegisterCodeRange`
- **THEN** the call returns `Ok`
- **AND** the data word is recorded as a non-executable data sentinel

#### Scenario: A load of embedded data reads the original bytes

- **WHEN** the cache track executes a range whose code loads from an embedded data word (e.g. an `LDR`-literal)
- **THEN** the loaded value is the original bytes of that word
- **AND** the loaded value matches the debug track

#### Scenario: Reaching a data-sentinel word on the cache track aborts

- **WHEN** cache-track execution reaches a word recorded as a data sentinel (a wild or deliberate branch into the data)
- **THEN** the Simulator aborts with a diagnostic naming the offending address
- **AND** it does not execute the data as an instruction or fall back to the imported decoder

#### Scenario: A range with embedded data executes identically on both tracks

- **WHEN** the same range with embedded read-only data is executed once via `RunFrom` and once via `DebugRunFrom` from the same initial state
- **THEN** the post-run register, flag, and memory state are identical
