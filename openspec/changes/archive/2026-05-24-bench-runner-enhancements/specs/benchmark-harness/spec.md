## ADDED Requirements

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

## MODIFIED Requirements

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
