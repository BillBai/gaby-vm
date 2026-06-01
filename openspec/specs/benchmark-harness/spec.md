# benchmark-harness Specification

## Purpose
Provide a developer-invoked harness that drives the imported VIXL AArch64
simulator through a fixed-size instruction workload, measures wall-clock
throughput, and reports instructions-per-second and ns-per-instruction in the
format laid out in `docs/refs/baseline-benchmark-suite.md`. This capability
owns the `bench/` source tree, the `GABY_VM_BUILD_BENCHMARKS` CMake option,
the workload header schema, and the reporting contract. It is the measurement
surface against which future optimization work — first the predecode/dispatch
cache, later any leaf-level changes — will land.
## Requirements
### Requirement: Benchmark build is gated by the GABY_VM_BUILD_BENCHMARKS CMake option

The top-level `CMakeLists.txt` SHALL declare a CMake option named
`GABY_VM_BUILD_BENCHMARKS`. The option SHALL default to
`${PROJECT_IS_TOP_LEVEL}` so that standalone builds opt in automatically and
embedded consumers opt in deliberately. The option SHALL gate the
`add_subdirectory(bench)` call so that, when the option is OFF, no benchmark
sources are compiled and no benchmark targets exist.

#### Scenario: Option defaults to ON for a top-level configure

- **WHEN** CMake is configured with no `GABY_VM_BUILD_BENCHMARKS` override and the project is the top-level project
- **THEN** `GABY_VM_BUILD_BENCHMARKS` resolves to `ON` and both `bench_baseline` and `bench_smoke` appear as build targets

#### Scenario: Option turned off removes all benchmark targets

- **WHEN** CMake is configured with `-DGABY_VM_BUILD_BENCHMARKS=OFF`
- **THEN** no target whose name starts with `bench_` exists in the configured build, and no source file under `bench/` is compiled

### Requirement: Two benchmark binaries are produced under bench/

When `GABY_VM_BUILD_BENCHMARKS` is ON, the build SHALL produce exactly two
executables under the `bench/` subdirectory:

- `bench_baseline` — driven by the committed mixed-workload header, used as
  the project's reference performance measurement.
- `bench_smoke` — driven by the committed smoke-workload header, used as a
  millisecond-scale end-to-end harness self-test.

Both executables SHALL share the timing loop, exit-convention setup, and
reporting code; they SHALL differ only in which workload header they
include.

#### Scenario: Both binaries build when the option is ON

- **WHEN** CMake is configured with `GABY_VM_BUILD_BENCHMARKS=ON` and the build is run
- **THEN** the build output directory contains a `bench_baseline` executable and a `bench_smoke` executable

#### Scenario: Shared runner is factored once

- **WHEN** `bench/CMakeLists.txt` is read
- **THEN** it defines a single shared compilation unit (object library or equivalent) for the timing-loop and reporting code that both executables consume, rather than duplicating that code per binary

### Requirement: Benchmark binaries are not registered with CTest

Neither `bench_baseline` nor `bench_smoke` SHALL be registered as a CTest
test via `add_test`. Benchmarks SHALL remain developer-invoked tools, kept
separate from the correctness suite that `ctest` runs.

#### Scenario: ctest does not list benchmark binaries

- **WHEN** `ctest -N` is run in a configured build directory with `GABY_VM_BUILD_BENCHMARKS=ON` and `GABY_VM_BUILD_TESTS=ON`
- **THEN** the listed test names do not include `bench_baseline` or `bench_smoke`

### Requirement: Workloads ship as committed C++ headers following a uniform schema

Each workload SHALL ship as a committed header at
`bench/workloads/<name>_workload_data.h`. The header SHALL declare
constants following a uniform schema so that the shared harness runner can
consume any workload without per-workload code paths:

- `inline constexpr uint32_t k<Name>WorkloadInstructions[]` — the AArch64
  instruction word stream that constitutes the workload body.
- `inline constexpr size_t k<Name>WorkloadStaticWordCount` — the number of
  4-byte instruction words physically present in the array.
- `inline constexpr uint64_t k<Name>WorkloadDynamicInstructionsPerIteration`
  — the number of guest instructions actually executed by a single
  `Simulator::RunFrom(start)` call over this workload. For workloads with
  no taken branches across the body, this value equals the static word
  count; for workloads with internal control flow it is captured offline
  during workload generation.
- `inline constexpr const char k<Name>WorkloadGeneratorTag[]` — a
  provenance string identifying the tool, version, and inputs used to
  produce the bytes (e.g., upstream-VIXL commit + RNG seed for mixed;
  `llvm-mc` version + source `.s` digest for smoke).

The build SHALL NOT regenerate these headers; they are committed source.

#### Scenario: Mixed workload header declares the full schema

- **WHEN** `bench/workloads/mixed_workload_data.h` is read
- **THEN** it declares `kMixedWorkloadInstructions`, `kMixedWorkloadStaticWordCount`, `kMixedWorkloadDynamicInstructionsPerIteration`, and `kMixedWorkloadGeneratorTag` with the types specified above

#### Scenario: Smoke workload header declares the full schema

- **WHEN** `bench/workloads/smoke_workload_data.h` is read
- **THEN** it declares `kSmokeWorkloadInstructions`, `kSmokeWorkloadStaticWordCount`, `kSmokeWorkloadDynamicInstructionsPerIteration`, and `kSmokeWorkloadGeneratorTag` with the types specified above

#### Scenario: Smoke workload's dynamic count equals its static count

- **WHEN** `bench/workloads/smoke_workload_data.h` is read
- **THEN** `kSmokeWorkloadDynamicInstructionsPerIteration` equals `kSmokeWorkloadStaticWordCount`

### Requirement: Harness writes LR to kEndOfSimAddress before each RunFrom call

The shared runner SHALL set the simulator's LR register to
`vixl::aarch64::Simulator::kEndOfSimAddress` before every invocation of
`Simulator::RunFrom(start)`, including the warm-up call. Workloads SHALL
follow the upstream VIXL convention of preserving LR across their bodies
so that the final `RET` returns control to the harness via the
`kEndOfSimAddress` sentinel.

#### Scenario: Runner writes LR before every RunFrom site

- **WHEN** the shared runner source file is read
- **THEN** every call site that invokes `simulator.RunFrom(...)` is immediately preceded by a write of `vixl::aarch64::Simulator::kEndOfSimAddress` to LR (e.g., `simulator.WriteLr(Simulator::kEndOfSimAddress)`)

#### Scenario: Both binaries terminate cleanly on the committed workloads

- **WHEN** either `bench_baseline` or `bench_smoke` is invoked with `--seconds 0.1`
- **THEN** the binary exits with status 0 and emits the full set of output lines (no hang, no abort, no segfault)

### Requirement: Steady-state timing loop discards a warm-up iteration and is bounded by wall-clock duration

The shared runner SHALL execute exactly one warm-up `RunFrom` call before
starting the timed region; the warm-up call's elapsed time and its
contribution to the iteration count SHALL NOT appear in the reported
output. After the warm-up, the runner SHALL enter a steady-state loop that
calls `RunFrom` repeatedly, counting iterations, until the elapsed
wall-clock time crosses a target duration. The target duration SHALL
default to 5 seconds, mirroring upstream `BenchCLI::kDefaultRunTime`. The
target duration SHALL be overridable at the command line via
`--seconds <float>`. The runner SHALL reject `--seconds` values strictly
less than `0.001` (1 millisecond) by writing an error to stderr that names
the minimum and exiting with status 2 before the warm-up call. Values
equal to or greater than the minimum SHALL be accepted unchanged.
Elapsed time SHALL be measured by a monotonic clock (e.g.,
`std::chrono::steady_clock`).

#### Scenario: Default duration is 5 seconds

- **WHEN** a benchmark binary is invoked with no arguments on a host able to complete at least one iteration in well under 5 seconds
- **THEN** the output's `elapsed_seconds` value is at least 5.0

#### Scenario: --seconds overrides the default

- **WHEN** a benchmark binary is invoked with `--seconds 1.0`
- **THEN** the output's `elapsed_seconds` value is at least 1.0 and substantially less than the default 5.0

#### Scenario: Warm-up is excluded from reported counts

- **WHEN** a benchmark binary is invoked
- **THEN** the reported `iterations` count is the number of `RunFrom` calls inside the timed region only, exclusive of the one warm-up call

#### Scenario: Below-minimum --seconds is rejected

- **WHEN** a benchmark binary is invoked with `--seconds 0.0005` (or any positive value strictly less than 0.001)
- **THEN** stderr contains an error message naming the `0.001` minimum
- **AND** the binary exits with status 2 without running the warm-up or timed loop

### Requirement: Output is plain-text key/value lines with iterations_per_second as the primary metric

Each benchmark invocation SHALL emit, to stdout, a fixed set of
`key: value` lines (one key-value pair per line). The output SHALL include
at least the following keys with the meanings given:

- `workload` — short string identifier of the workload (e.g., `mixed`, `smoke`).
- `build_type` — the CMake configuration the runner was compiled with
  (e.g., `Release`, `Debug`, `RelWithDebInfo`, `MinSizeRel`). The value is
  fixed at compile time; an unconfigured build (no active config) SHALL
  emit the string `Unknown` rather than an empty value, so every output
  block is greppable for `build_type:`.
- `workload_generator_tag` — the workload header's provenance string.
- `static_words_in_buffer` — `k<Name>WorkloadStaticWordCount`.
- `dynamic_instructions_per_iteration` — `k<Name>WorkloadDynamicInstructionsPerIteration`.
- `iterations` — count of `RunFrom` calls in the timed region (warm-up excluded).
- `total_dynamic_instructions` — `iterations × dynamic_instructions_per_iteration`.
- `elapsed_seconds` — wall-clock duration of the timed region.
- `iterations_per_second` — the **primary** metric, defined as
  `iterations / elapsed_seconds`, matching upstream `BenchCLI::PrintResults`.
- `throughput_insn_per_sec` — derived as
  `total_dynamic_instructions / elapsed_seconds`.
- `ns_per_instruction` — derived as
  `elapsed_seconds × 1e9 / total_dynamic_instructions`.

All derived metrics that reference an instruction count SHALL use the
recorded dynamic count, NOT the static word count.

#### Scenario: All required keys are present in the output

- **WHEN** a benchmark binary is invoked
- **THEN** stdout contains at least one line for each of the keys listed above, each formatted as `<key>: <value>`

#### Scenario: iterations_per_second matches iterations / elapsed_seconds

- **WHEN** the output of an invocation is parsed
- **THEN** the `iterations_per_second` value equals `iterations / elapsed_seconds` within floating-point rounding tolerance

#### Scenario: ns_per_instruction uses dynamic count, not static count

- **WHEN** the output of a `bench_baseline` invocation is parsed (a workload where dynamic ≠ static)
- **THEN** the `ns_per_instruction` value equals `elapsed_seconds × 1e9 / (iterations × dynamic_instructions_per_iteration)` within floating-point rounding, and is inconsistent with the result of substituting `static_words_in_buffer` for `dynamic_instructions_per_iteration`

#### Scenario: build_type reflects the CMake configuration

- **WHEN** a benchmark binary built with `-DCMAKE_BUILD_TYPE=Release` (single-config) or selected as `--config Release` (multi-config) is invoked
- **THEN** the output contains a `build_type: Release` line
- **AND** the same binary built with `-DCMAKE_BUILD_TYPE=Debug` instead emits `build_type: Debug`

### Requirement: bench/README.md documents build, invocation, workload generation, and upstream comparison

The repository SHALL contain `bench/README.md`. The README SHALL document,
at minimum:

- How to configure and build the project with `GABY_VM_BUILD_BENCHMARKS=ON`.
- How to invoke each benchmark binary and a brief explanation of each
  output key, including `build_type`.
- The supported command-line flags, including `--help` / `-h`, the
  `--seconds <float>` flag, and the `0.001` minimum that `--seconds`
  enforces.
- The offline procedure for regenerating `mixed_workload_data.h` using
  upstream VIXL's `BenchCodeGenerator` together with an instruction-count
  hook (e.g., `set_trace_parameters`).
- The offline procedure for regenerating `smoke_workload_data.h` using
  `llvm-mc` (or equivalent assembler) and `llvm-objcopy` (or equivalent
  byte extractor).
- The manual procedure for cross-running an equivalent workload against
  upstream VIXL's `bench-mixed-sim` for comparison.

The README SHALL NOT prescribe rigorous noise-control protocols (core
pinning, turbo disabling, governor tweaks); a brief mention of
common-sense host hygiene ("prefer a mostly-idle machine, don't run on
battery") is sufficient because V1 targets order-of-magnitude correctness,
not publication-grade measurements.

#### Scenario: README covers each documented procedure

- **WHEN** `bench/README.md` is read
- **THEN** it contains content covering each of the following topics, identifiable by headings or clearly labeled sections: build invocation, per-binary usage and output meaning (including `build_type`), supported flags (including `--help`/`-h` and the `--seconds` minimum), mixed-workload regeneration, smoke-workload regeneration, and upstream cross-comparison

### Requirement: Benchmark sources do not modify Sources/gaby_vm/src/, Sources/gaby_vm/include/gaby_vm/, or imported VIXL files

The benchmark harness SHALL NOT alter any file under `Sources/gaby_vm/src/`, any imported
VIXL source file, or any public header under `Sources/gaby_vm/include/gaby_vm/`. The harness
MAY consume the `gaby_vm` public API — including the `PredecodeCache` and
`Simulator` types introduced by the `predecode-cache` capability — in addition
to the imported `Sources/gaby_vm/src/` headers it already uses to drive
`vixl::aarch64::Simulator`. The harness itself SHALL NOT introduce any new
public header or other new public API surface.

#### Scenario: Benchmark sources are confined to bench/ and top-level CMakeLists.txt

- **WHEN** the set of files owned by this capability is enumerated
- **THEN** every owned file path is one of: `CMakeLists.txt` at the repo root, or a file under `bench/`

#### Scenario: No new public headers are introduced

- **WHEN** the contents of `Sources/gaby_vm/include/gaby_vm/` are listed
- **THEN** the listing contains no public header added by the benchmark-harness capability (cache headers added by the `predecode-cache` capability are consumed by the harness, not owned by it)

### Requirement: Help flag prints usage and exits successfully

Each benchmark binary SHALL accept `--help` and `-h` as command-line flags.
When invoked with either flag — at any position among the arguments — the
binary SHALL emit, to stdout, a usage block that names the binary,
enumerates every supported flag (currently `--seconds <float>`, `--help`,
`-h`), and states the default value for `--seconds`. The binary SHALL then
exit with status 0 and SHALL NOT run the warm-up `RunFrom`, the timed
loop, or any other simulator work. The unknown-argument error path SHALL
direct the user to `--help` so the flag is discoverable from the binary's
own error output.

#### Scenario: --help prints usage and exits 0

- **WHEN** a benchmark binary is invoked with `--help`
- **THEN** stdout contains a usage block that names every supported flag (at minimum `--seconds`, `--help`, `-h`) and the binary exits with status 0
- **AND** no `iterations:` line, `elapsed_seconds:` line, or other steady-state output appears

#### Scenario: -h is a synonym for --help

- **WHEN** a benchmark binary is invoked with `-h`
- **THEN** the behavior matches the `--help` scenario above (same stdout content, exit status 0, no timed-loop output)

#### Scenario: Unknown argument error references --help

- **WHEN** a benchmark binary is invoked with an unknown argument such as `--bogus`
- **THEN** stderr contains an error naming the offending argument and referring the user to `--help` for the supported flag list
- **AND** the binary exits with status 2

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

