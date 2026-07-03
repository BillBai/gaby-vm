# Delta: predecode-cache (cache-dispatch-devirt)

## ADDED Requirements

### Requirement: Cache-track dispatch is a single-indirection handler call

The cache-track per-instruction dispatch SHALL invoke the predecoded
instruction's handler with at most one dependent pointer load from the
`PredecodedEntry` (the handler function pointer stored in the entry's
third slot) followed by one indirect call. The stored handler SHALL be an
ordinary AOT-compiled function; predecoded data remains non-executable
ordinary data. The `PredecodedEntry` layout scenario is unchanged: 16
bytes, same field offsets and types — the third slot (`leaf`) now holds
the resolved handler function pointer rather than a pointer to
pointer-to-member-function storage.

#### Scenario: Dispatch does not pay pmf or vtable indirection

- **WHEN** the cache-track dispatch of a predecoded instruction is
  inspected at the source level
- **THEN** the path from `PredecodedEntry` to the executed leaf contains
  one load of the entry's handler slot and one indirect call, with no
  pointer-to-member-function load and no vtable dispatch

#### Scenario: Handler execution is byte-equivalent to the previous hub

- **WHEN** any registered range executes on the cache track before and
  after the handler migration
- **THEN** architectural end state is identical, and the per-step protocol
  side effects (`form_hash_` seating, BTI gate, MOVPRFX latch and check,
  `last_instr_`, PC advance, BType update) occur in the same order with
  the same observable behavior
