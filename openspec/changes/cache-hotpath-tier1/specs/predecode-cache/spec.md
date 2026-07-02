# Delta: predecode-cache (cache-hotpath-tier1)

## ADDED Requirements

### Requirement: MOVPRFX classification is predecode-derived and the protocol check remains enforced

The cache track SHALL derive "this instruction is an SVE `MOVPRFX`" from a
per-entry classification bit written by the predecode pass (in the existing
32-bit `flags` slot), and SHALL NOT re-derive it per execution from
`form_hash_` comparisons. The `CanTakeSVEMovprfx` protocol check SHALL
continue to abort execution when the instruction following a `MOVPRFX` is
not a legal consumer, with behavior identical to the debug track. The
previous-instruction-was-MOVPRFX state SHALL be part of the re-entrancy
cursor: a nested execution call SHALL NOT corrupt the enclosing run's
MOVPRFX chain.

#### Scenario: Legal MOVPRFX pair executes identically on both tracks

- **WHEN** a hand-encoded `MOVPRFX` followed by a legal consumer instruction
  is registered and executed on the cache track and on the debug track at
  VL=128
- **THEN** both tracks execute the pair without aborting
- **AND** the resulting architectural state is identical across tracks

#### Scenario: Illegal MOVPRFX consumer aborts on the cache track

- **WHEN** a hand-encoded `MOVPRFX` followed by an instruction that is not a
  legal consumer is executed on the cache track
- **THEN** execution aborts via the `CanTakeSVEMovprfx` check, as it does on
  the debug track

#### Scenario: MOVPRFX classification is not recomputed per execution

- **WHEN** the cache-track dispatch of a non-MOVPRFX instruction is
  inspected
- **THEN** the per-step path reads the predecoded classification bit and
  performs no `form_hash_`-based MOVPRFX comparison
