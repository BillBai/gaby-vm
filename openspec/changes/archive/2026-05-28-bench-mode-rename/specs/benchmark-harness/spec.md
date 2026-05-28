## REMOVED Requirements

### Requirement: Benchmark harness supports a cache execution engine

**Reason**: terminology rename. The behavioural surface — selectable
at invocation, pre-timing registration, output identification,
single-binary constraint — is re-stated verbatim under the new title
in the `ADDED Requirements` section below. The user-visible
vocabulary changes from "engine" to "mode" so that code
(`enum class Mode`), CLI (`--mode {decoder|cache}`), output
(`mode:` key), prose (`bench/README.md`, `docs/architecture.md`,
`docs/conventions.md`), and this spec all use the same word for the
same thing.

**Migration**: any consumer that spelled `--engine`, `engine:`, or
"engine" prose updates to `--mode`, `mode:`, and "mode" prose. No
behavioural migration is required.

## ADDED Requirements

### Requirement: Benchmark harness supports a cache execution mode

The benchmark harness SHALL let the invoker select which execution mode
drives a workload: a decoder mode (the imported `vixl::aarch64::Simulator`,
the existing default) or a cache mode (`gaby_vm::Simulator` over a
`gaby_vm::PredecodeCache`). When the cache mode is selected, the harness
SHALL register the workload's instruction buffer as a code range with the
cache before the timed region and SHALL drive execution through the cache
track. The cache mode SHALL observe the same warm-up, steady-state timing,
and LR-termination conventions as the decoder mode, so that metrics reported
for the two modes are directly comparable. The harness output SHALL identify
which mode produced the run. Mode selection SHALL NOT add a new benchmark
binary — both modes run from the existing `bench_baseline` and `bench_smoke`
executables.

#### Scenario: Mode is selectable at invocation

- **WHEN** a benchmark binary is invoked with the cache mode selected
- **THEN** the workload runs through the `gaby_vm::Simulator` cache track
- **AND** when the binary is invoked with the decoder mode selected, or with no mode selection, the workload runs through the imported `vixl::aarch64::Simulator`

#### Scenario: Cache mode registers the workload before timing

- **WHEN** the cache mode runs a workload
- **THEN** the workload instruction buffer is registered as a code range with the `PredecodeCache` before the warm-up call, so the timed region measures steady-state cache execution only

#### Scenario: Output identifies the mode

- **WHEN** a benchmark binary completes a run
- **THEN** its plain-text key/value output includes a key that identifies which execution mode produced the metrics

#### Scenario: Mode selection adds no benchmark binary

- **WHEN** the build is configured with `GABY_VM_BUILD_BENCHMARKS=ON`
- **THEN** the only benchmark executables are `bench_baseline` and `bench_smoke`, each able to run either mode
