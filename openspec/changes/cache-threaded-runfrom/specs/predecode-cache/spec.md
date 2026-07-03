# Delta: predecode-cache (cache-threaded-runfrom)

## ADDED Requirements

### Requirement: Threaded execution is observationally identical to stepped execution

The cache track MAY execute a run as handler-to-handler tail-call chains
with no per-instruction central loop. Threaded execution SHALL be
observationally identical to stepping the same range one instruction at a
time: identical architectural end state, identical per-instruction protocol
side effects, and identical abort behavior. A runtime step-mode gate SHALL
let the same compiled handler bodies execute exactly one instruction per
call for the stepping APIs (`StepOnce`, `ShadowRunner`); the gate is
harness state saved and restored across nested execution calls, and is not
part of the re-entrancy cursor.

#### Scenario: Whole-run equivalence across threaded, stepped, and decoder execution

- **WHEN** the same registered range is executed to completion via threaded
  `RunFrom`, via repeated `StepOnce`, and via the debug track
- **THEN** the architectural end states of all three are identical

#### Scenario: A hook-terminated run ends cleanly from within a chain

- **WHEN** a branch hook (or `RET` to a null link register) sets the PC to
  the end-of-simulation sentinel while a threaded chain is executing
- **THEN** the chain terminates and `RunFrom` returns normally, without a
  range-lookup abort

#### Scenario: Nested execution from a hook inside a chain

- **WHEN** a branch hook invoked from a threaded chain starts a nested
  execution call on the same Simulator and that call completes
- **THEN** the enclosing chain continues from the enclosing run's PC with
  its cursor state intact, as required by the re-entrancy requirement; the
  enclosing continuation is derived from the interpreter state (`pc_`)
  after the nested call, not from any stale pre-call value

### Requirement: Every registered range carries a boundary sentinel entry

The predecode pass SHALL allocate one entry beyond the range's last
instruction word (`size_bytes/4 + 1` entries). The boundary entry's handler
SHALL re-resolve the PC: straight-line execution that walks off the end of
a range SHALL continue into an adjacently registered range, terminate
cleanly on the end-of-simulation sentinel, and abort on a PC outside every
registered range — preserving the existing fallthrough and abort contracts
without a per-instruction range containment check.

#### Scenario: Straight-line fallthrough into an adjacent range

- **WHEN** two ranges are registered back-to-back and execution runs off
  the end of the first without a branch
- **THEN** execution continues at the first instruction of the second
  range with identical architectural results on the threaded and stepped
  representations

#### Scenario: Running off the end into unregistered memory aborts

- **WHEN** straight-line execution passes the last word of a range and no
  registered range covers the next address
- **THEN** execution aborts identifying the out-of-range PC, on both
  representations
