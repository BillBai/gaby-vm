## ADDED Requirements

### Requirement: Benchmark harness supports a cache execution engine

The benchmark harness SHALL let the invoker select which execution engine
drives a workload: a decoder engine (the imported `vixl::aarch64::Simulator`,
the existing default) or a cache engine (`gaby_vm::Simulator` over a
`gaby_vm::PredecodeCache`). When the cache engine is selected, the harness
SHALL register the workload's instruction buffer as a code range with the
cache before the timed region and SHALL drive execution through the cache
track. The cache engine SHALL observe the same warm-up, steady-state timing,
and LR-termination conventions as the decoder engine, so that metrics reported
for the two engines are directly comparable. The harness output SHALL identify
which engine produced the run. Engine selection SHALL NOT add a new benchmark
binary — both engines run from the existing `bench_baseline` and `bench_smoke`
executables.

#### Scenario: Engine is selectable at invocation

- **WHEN** a benchmark binary is invoked with the cache engine selected
- **THEN** the workload runs through the `gaby_vm::Simulator` cache track
- **AND** when the binary is invoked with the decoder engine selected, or with no engine selection, the workload runs through the imported `vixl::aarch64::Simulator`

#### Scenario: Cache engine registers the workload before timing

- **WHEN** the cache engine runs a workload
- **THEN** the workload instruction buffer is registered as a code range with the `PredecodeCache` before the warm-up call, so the timed region measures steady-state cache execution only

#### Scenario: Output identifies the engine

- **WHEN** a benchmark binary completes a run
- **THEN** its plain-text key/value output includes a key that identifies which execution engine produced the metrics

#### Scenario: Engine selection adds no benchmark binary

- **WHEN** the build is configured with `GABY_VM_BUILD_BENCHMARKS=ON`
- **THEN** the only benchmark executables are `bench_baseline` and `bench_smoke`, each able to run either engine

## MODIFIED Requirements

### Requirement: Benchmark sources do not modify src/, include/gaby_vm/, or imported VIXL files

The benchmark harness SHALL NOT alter any file under `src/`, any imported
VIXL source file, or any public header under `include/gaby_vm/`. The harness
MAY consume the `gaby_vm` public API — including the `PredecodeCache` and
`Simulator` types introduced by the `predecode-cache` capability — in addition
to the imported `src/` headers it already uses to drive
`vixl::aarch64::Simulator`. The harness itself SHALL NOT introduce any new
public header or other new public API surface.

#### Scenario: Benchmark sources are confined to bench/ and top-level CMakeLists.txt

- **WHEN** the set of files owned by this capability is enumerated
- **THEN** every owned file path is one of: `CMakeLists.txt` at the repo root, or a file under `bench/`

#### Scenario: No new public headers are introduced

- **WHEN** the contents of `include/gaby_vm/` are listed
- **THEN** the listing contains no public header added by the benchmark-harness capability (cache headers added by the `predecode-cache` capability are consumed by the harness, not owned by it)
