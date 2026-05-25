## MODIFIED Requirements

### Requirement: RegisterCodeRange validates inputs and reports failures all-or-nothing

`RegisterCodeRange` SHALL report its outcome through a
`PredecodeCache::RegistrationStatus` value. It SHALL return `OverlappingRange`
when the requested range overlaps any already-registered range in any way,
including an exact duplicate. It SHALL return `UnsupportedFeature` when any
instruction in the range requires a CPU feature the cache's
`CPUFeaturesAuditor` does not accept. It SHALL return `InvalidArgument` when
the size is zero or not a multiple of 4. It SHALL return `OutOfMemory` when
entry storage cannot be allocated. On any non-`Ok` result the call SHALL
register nothing — no instruction from a failed call SHALL become executable
through the cache. When the result is `UnsupportedFeature`, the cache SHALL
expose a queryable `PredecodeCache::RegistrationError` that identifies the
offending instruction address and the missing CPU feature(s).

#### Scenario: Overlapping range is rejected

- **WHEN** a range that overlaps an already-registered range is passed to `RegisterCodeRange`
- **THEN** the call returns `OverlappingRange`
- **AND** the set of cache-executable instructions is unchanged

#### Scenario: Unsupported feature is reported with diagnostic detail

- **WHEN** a range contains an instruction that requires a CPU feature the auditor rejects
- **THEN** `RegisterCodeRange` returns `UnsupportedFeature`
- **AND** the cache's `PredecodeCache::RegistrationError` names the offending instruction address and the missing feature(s)

#### Scenario: Malformed size is rejected

- **WHEN** `RegisterCodeRange` is called with a size that is zero or not a multiple of 4
- **THEN** it returns `InvalidArgument` and registers nothing

#### Scenario: A failed registration is all-or-nothing

- **WHEN** a `RegisterCodeRange` call returns any non-`Ok` status
- **THEN** no instruction word from that call is executable through the cache track
