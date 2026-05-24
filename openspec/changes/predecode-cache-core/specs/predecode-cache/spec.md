## ADDED Requirements

### Requirement: PredecodeCache is a standalone, shareable cache object

The project SHALL provide a `gaby_vm::PredecodeCache` type, owned and
lifetime-managed by the embedder, that holds predecoded dispatch data for one
or more registered code ranges. A single `PredecodeCache` instance SHALL be
usable by multiple `gaby_vm::Simulator` instances at the same time.
`RegisterCodeRange` calls SHALL be serialized internally, and calling
`RegisterCodeRange` on one thread SHALL NOT corrupt or disrupt the lookups
performed by Simulators executing already-registered ranges on other threads.
`PredecodeCache` SHALL NOT be copyable.

#### Scenario: Cache releases its storage on destruction

- **WHEN** an embedder constructs a `PredecodeCache`, registers ranges, and later destroys it
- **THEN** all entry storage for the registered ranges is released
- **AND** no `gaby_vm::Simulator` may use the cache after it is destroyed

#### Scenario: One cache serves multiple Simulators

- **WHEN** two `gaby_vm::Simulator` instances are constructed against the same `PredecodeCache` pointer
- **THEN** both execute from the same registered ranges with no per-instance copy of the predecoded data

#### Scenario: Registration is safe to interleave with execution

- **WHEN** `RegisterCodeRange` is called on one thread while another thread's `Simulator` is executing an already-registered range
- **THEN** the executing `Simulator` continues correctly and is not corrupted by the concurrent registration
- **AND** once the call returns, the newly registered range is usable

#### Scenario: Cache is not copyable

- **WHEN** the `PredecodeCache` type is inspected
- **THEN** its copy constructor and copy-assignment operator are deleted

### Requirement: RegisterCodeRange predecodes a code range and is append-only

`PredecodeCache::RegisterCodeRange` SHALL accept a start address and a size in
bytes and SHALL predecode every 4-byte instruction word in that range so that
subsequent cache-track execution over the range dispatches through the cache.
`RegisterCodeRange` SHALL be callable at any point in the cache's lifetime,
including after `gaby_vm::Simulator` instances have been constructed and have
executed. The cache SHALL be append-only: a successfully registered range
SHALL remain valid until the cache is destroyed, and the V1 public API SHALL
NOT expose any operation that unregisters, flushes, or invalidates a
registered range.

#### Scenario: A registered range executes through the cache

- **WHEN** a code range is registered and a `Simulator` calls `RunFrom` at an address inside it
- **THEN** the instructions in that range dispatch through the predecode cache rather than the imported decoder

#### Scenario: Ranges may be registered after execution has begun

- **WHEN** a second code range is registered after a `Simulator` has already executed a first range
- **THEN** that `Simulator` can branch into and execute the second range
- **AND** the first range remains valid

#### Scenario: No unregister or flush operation exists

- **WHEN** the `PredecodeCache` public API is inspected
- **THEN** it exposes no method that removes, flushes, or invalidates an already-registered range

### Requirement: RegisterCodeRange validates inputs and reports failures all-or-nothing

`RegisterCodeRange` SHALL report its outcome through a `RegisterStatus` value.
It SHALL return `OverlappingRange` when the requested range overlaps any
already-registered range in any way, including an exact duplicate. It SHALL
return `UnsupportedFeature` when any instruction in the range requires a CPU
feature the cache's `CPUFeaturesAuditor` does not accept. It SHALL return
`InvalidArgument` when the size is zero or not a multiple of 4. It SHALL
return `OutOfMemory` when entry storage cannot be allocated. On any non-`Ok`
result the call SHALL register nothing — no instruction from a failed call
SHALL become executable through the cache. When the result is
`UnsupportedFeature`, the cache SHALL expose a queryable `ErrorDetail` that
identifies the offending instruction address and the missing CPU feature(s).

#### Scenario: Overlapping range is rejected

- **WHEN** a range that overlaps an already-registered range is passed to `RegisterCodeRange`
- **THEN** the call returns `OverlappingRange`
- **AND** the set of cache-executable instructions is unchanged

#### Scenario: Unsupported feature is reported with diagnostic detail

- **WHEN** a range contains an instruction that requires a CPU feature the auditor rejects
- **THEN** `RegisterCodeRange` returns `UnsupportedFeature`
- **AND** the cache's `ErrorDetail` names the offending instruction address and the missing feature(s)

#### Scenario: Malformed size is rejected

- **WHEN** `RegisterCodeRange` is called with a size that is zero or not a multiple of 4
- **THEN** it returns `InvalidArgument` and registers nothing

#### Scenario: A failed registration is all-or-nothing

- **WHEN** a `RegisterCodeRange` call returns any non-`Ok` status
- **THEN** no instruction word from that call is executable through the cache track

### Requirement: gaby_vm::Simulator exposes two non-switching execution tracks

`gaby_vm::Simulator` SHALL provide a cache track — `RunFrom` and `StepOnce` —
that dispatches through the `PredecodeCache`, and a debug track —
`DebugRunFrom` and `DebugStepOnce` — that dispatches through the imported
`Decoder → VisitNamedInstruction → leaf` flow. A single execution call SHALL
remain on one track for its entire duration; the simulator SHALL NOT switch
tracks within a run. For identical registered code and identical initial
state, the two tracks SHALL produce identical architectural results — general
registers, FP/SIMD registers, PC, NZCV, FPCR, FPSR, and memory. Trace,
debugger, and custom decoder-visitor configuration SHALL take effect only on
the debug track and SHALL be ignored by the cache track.

#### Scenario: Cache track dispatches through the cache

- **WHEN** `RunFrom` is called on a `Simulator` with a non-null cache at an entry address inside a registered range
- **THEN** the instructions execute through the predecode cache, not the imported decoder

#### Scenario: Debug track dispatches through the imported flow

- **WHEN** `DebugRunFrom` is called
- **THEN** the instructions execute through the imported `Decoder → VisitNamedInstruction → leaf` flow

#### Scenario: The two tracks agree

- **WHEN** the same code and initial register state are executed once via `RunFrom` and once via `DebugRunFrom`
- **THEN** the post-run register, flag, and memory state are identical

#### Scenario: Trace configuration affects only the debug track

- **WHEN** trace parameters are set and `RunFrom` is then called
- **THEN** the cache run produces no trace output
- **AND** when `DebugRunFrom` is called with the same trace parameters, the trace output is produced

### Requirement: A Simulator constructed without a cache supports only the debug track

`gaby_vm::Simulator` SHALL accept a null `PredecodeCache` at construction. A
`Simulator` constructed with a null cache SHALL fully support the debug track.
Invoking the cache track on a `Simulator` with a null cache SHALL fail
deterministically with a diagnostic abort rather than producing undefined
behavior.

#### Scenario: A cache-less Simulator runs the debug track

- **WHEN** a `Simulator` is constructed with a null cache and `DebugRunFrom` is called
- **THEN** the code executes through the imported decoder flow and terminates normally

#### Scenario: The cache track on a cache-less Simulator aborts

- **WHEN** `RunFrom` or `StepOnce` is called on a `Simulator` constructed with a null cache
- **THEN** the simulator aborts with a diagnostic instead of dereferencing the null cache

### Requirement: RunFrom aborts on a PC outside every registered range

The `Simulator` SHALL abort when cache-track execution reaches an instruction
address that lies inside no registered code range, and the abort diagnostic
SHALL identify the offending address. The cache track SHALL NOT silently fall
back to the imported decoder for an out-of-range address.

#### Scenario: An out-of-range PC aborts

- **WHEN** cache-track execution reaches an address outside every registered range
- **THEN** the simulator aborts and the message names the offending address

#### Scenario: No silent fallback to the decoder

- **WHEN** an out-of-range address is reached on the cache track
- **THEN** execution does not continue through the imported decoder

### Requirement: ShadowRunner detects per-instruction divergence between the two tracks

The project SHALL provide a `gaby_vm::testing::ShadowRunner` that executes the
cache track and the debug track in lockstep over the same code, using a shared
stack buffer so register values are directly comparable, and compares
architectural state after each instruction. The compared state SHALL include
the general registers X0–X30 and SP, the FP/SIMD registers V0–V31 compared
over their full 128 bits, PC, NZCV, FPCR, FPSR, BType, and the memory writes
performed by the instruction. On the first divergence, `ShadowRunner` SHALL
build a `DivergenceReport` and deliver it to a divergence handler. The default
handler SHALL abort; the embedder SHALL be able to install a custom handler
that receives the report instead.

#### Scenario: A matching run reports no divergence

- **WHEN** `ShadowRunner` runs code over which the cache track and the debug track agree at every instruction
- **THEN** it completes without producing a `DivergenceReport`

#### Scenario: Register divergence is detected

- **WHEN** the two tracks produce different register, flag, or PC state for some instruction
- **THEN** `ShadowRunner` produces a `DivergenceReport` identifying the instruction address and the differing field

#### Scenario: Memory-write divergence is detected

- **WHEN** the two tracks perform different memory writes for some instruction
- **THEN** `ShadowRunner` produces a `DivergenceReport` identifying the instruction address and the differing write

#### Scenario: A custom divergence handler receives the report

- **WHEN** a custom divergence handler is installed and a divergence occurs
- **THEN** the handler is invoked with the `DivergenceReport` instead of the default abort

### Requirement: Execution calls are re-entrant and restore the enclosing run's cursor

`RunFrom`, `DebugRunFrom`, `StepOnce`, and `DebugStepOnce` SHALL be re-entrant
on a single `Simulator`: an execution call made while another execution call
on the same `Simulator` is in progress — for example, from a native bridge
callback invoked inside a leaf — SHALL save the enclosing run's interpreter
cursor on entry and restore it on return. The cursor comprises the
interpreter's run-scoped loop and decode state (the program counter, the cache
`cur_range_`, the last decoded form hash, and the last-executed-instruction
record); it SHALL NOT include the guest register file, which is shared mutable
state that flows across calls. A nested call MAY use either execution track
regardless of the enclosing call's track. No separate `Simulator` instance
SHALL be required for re-entry.

#### Scenario: A nested execution call restores the enclosing cursor

- **WHEN** an execution call is made on a `Simulator` from within a leaf of an in-progress execution call on the same `Simulator`
- **THEN** the nested call runs to completion
- **AND** on its return the enclosing run resumes at the correct instruction with its cursor intact

#### Scenario: Guest registers flow across a nested call

- **WHEN** a nested execution call mutates the guest register file
- **THEN** those mutations remain visible to the enclosing run after the nested call returns — the register file is not saved or restored by the nesting

#### Scenario: A nested call may use either track

- **WHEN** the enclosing call is on the cache track and a nested call is made on the debug track, or vice versa
- **THEN** both calls execute on their respective tracks and the enclosing call's cursor is restored on return

### Requirement: Dual-path correctness and the shadow oracle are registered with CTest

The repository SHALL register a CTest case that drives hand-encoded AArch64
instruction sequences through both `gaby_vm::Simulator` tracks — `RunFrom` and
`DebugRunFrom` — and asserts that each track reaches the precomputed expected
post-run state. That case SHALL cover at least the baseline instruction
families: integer arithmetic, logical, load/store, conditional control flow,
and procedure call/return. The repository SHALL also register a CTest case for
`ShadowRunner` that verifies the oracle reports no divergence on a matching
run AND verifies, through a deliberately injected cache-track defect, that a
divergence is actually detected. A run in which either track disagrees with
the expected state, or in which the injected defect goes undetected, SHALL
fail the corresponding CTest case with diagnostic output naming the failure.

#### Scenario: The dual-path correctness case is registered

- **WHEN** `ctest -N` is run in a configured build directory
- **THEN** a correctness test case appears that exercises both the `RunFrom` and `DebugRunFrom` tracks

#### Scenario: Baseline families are exercised on both tracks

- **WHEN** the dual-path correctness test runs
- **THEN** each baseline family — integer arithmetic, logical, load/store, conditional control flow, and procedure call/return — is executed on both tracks and asserted against the precomputed expected state

#### Scenario: The shadow oracle case is registered

- **WHEN** `ctest -N` is run in a configured build directory
- **THEN** a `ShadowRunner` self-test case appears

#### Scenario: An injected cache-track defect is caught

- **WHEN** the `ShadowRunner` self-test runs its injected-defect sub-case
- **THEN** `ShadowRunner` produces a `DivergenceReport` for the injected defect
- **AND** the sub-case passes only because the divergence was detected — an undetected divergence fails the test
