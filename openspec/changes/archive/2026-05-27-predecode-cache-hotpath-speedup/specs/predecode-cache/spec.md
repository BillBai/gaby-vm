## ADDED Requirements

### Requirement: Cache-track per-instruction dispatch amortizes encoding-derived work

The cache-track per-instruction dispatch SHALL move encoding-derived
per-step work into the predecode pass and SHALL NOT repeat that work on
each execution of an already-predecoded instruction. Specifically,
per-instruction operations whose result depends ONLY on the
instruction's encoding — leaf resolution, form classification,
BTI / guarded-page relevance — SHALL be performed at most once per
instruction, at predecode time, and SHALL NOT be evaluated again on
each cache-track execution of that instruction. The cache-track
per-instruction non-leaf overhead SHALL therefore be independent of
the workload's instruction mix.

The runtime hot path MAY still perform per-instruction work whose
outcome depends on dynamic state (e.g. the actual leaf execution; BType
enforcement on a BTI-flagged instruction; reentrancy bookkeeping). It
SHALL NOT perform per-instruction work whose outcome is fully
determined by the encoding alone.

#### Scenario: Leaf dispatch does not pay a type-erasure indirection

- **WHEN** the cache track executes an instruction whose leaf has been
  predecoded
- **THEN** the dispatch invokes the leaf directly through the predecoded
  handle, without an intermediate `std::function` or other type-erased
  wrapper call

#### Scenario: BTI / guarded-page enforcement is gated by predecode-time classification

- **WHEN** the cache track executes an instruction that the predecode
  pass has not classified as BTI-relevant (e.g. an integer ALU
  instruction)
- **THEN** the per-step path performs no BType comparison or guarded-
  page check for that instruction
- **AND** when the cache track executes an instruction that IS
  classified as BTI-relevant (e.g. `BTI`, `PACIASP`, `PACIBSP`, `BRK`,
  `HLT`, or an exception-causing form), the BType / guarded-page
  enforcement runs and matches the architectural behavior of the debug
  track for that instruction

#### Scenario: ShadowRunner remains divergence-free across the optimization

- **WHEN** ShadowRunner executes the cache track and the debug track in
  lockstep over any workload — including a sequence that exercises
  `BTI` / `PACI[AB]SP` interaction
- **THEN** no register, flag, PC, or memory-write divergence is reported

#### Scenario: Predecoded entry size and offsets are unchanged

- **WHEN** the layout of `gaby_vm::PredecodeCache::PredecodedEntry` is
  inspected
- **THEN** its size remains 16 bytes and the offsets and types of its
  three fields (`form_hash`, the 32-bit hot-path classification slot,
  and `leaf`) are unchanged — the optimization uses bits within the
  existing 32-bit slot rather than adding a new field
- **AND** the 32-bit hot-path classification slot is named `flags`
  rather than `reserved`, reflecting its real per-entry classification
  role (the rename is the only public-header change introduced by
  this optimization)
