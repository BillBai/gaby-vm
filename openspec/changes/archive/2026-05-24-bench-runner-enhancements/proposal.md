## Why

The `bench/` harness has three small but real usability and reproducibility
gaps: invoking it with `--help` prints `unknown argument: --help` (the only
documented flag is undiscoverable from the binary itself), the output gives
no indication whether it was built `Release` or `Debug` (a 10× perf trap when
sharing numbers), and `--seconds` accepts arbitrarily small positive values
like `1e-9` that produce meaningless results. Closing all three before the
predecode-cache work lands keeps the baseline numbers we will be comparing
against trustworthy.

## What Changes

- Add `--help` / `-h`: prints a usage block listing supported flags to
  stdout, then exits 0. Triggered before any other parsing.
- Reject `--seconds` values below a documented minimum (1 ms = `0.001`).
  Print an error to stderr referencing the minimum, exit 2 (existing
  arg-error convention).
- Emit a new output line `build_type: <Release|Debug|RelWithDebInfo|
  MinSizeRel|...>` driven from the CMake configuration that compiled the
  runner. Position: at the top of the output block alongside `workload`.
- Improve the unknown-argument error path to direct the user to `--help`.
- Update `bench/README.md` to document `--help`, the `--seconds` minimum,
  and the new `build_type` output key.

## Capabilities

### New Capabilities

None. All changes extend the existing `benchmark-harness` capability.

### Modified Capabilities

- `benchmark-harness`: extends the CLI contract (new `--help`/`-h`, new
  `--seconds` lower bound) and the output schema (new `build_type` key);
  README requirement broadens to cover the new flag and key.

## Impact

- `bench/runner.cc`: `ParseArgs` gains `--help`/`-h` handling and a
  minimum-seconds check; main path emits the new `build_type` line.
- `bench/CMakeLists.txt`: adds
  `target_compile_definitions(bench_runner PRIVATE
  GABY_VM_BENCH_BUILD_TYPE="$<CONFIG>")` so the runner can stringize the
  active CMake configuration at compile time.
- `bench/README.md`: documents the new flag, the minimum, and the new
  output key.
- `openspec/specs/benchmark-harness/spec.md`: receives modified
  requirements via the change's spec delta (will land under
  `openspec/changes/bench-runner-enhancements/specs/benchmark-harness/spec.md`).

No changes to `src/`, `include/gaby_vm/`, or any imported VIXL file —
staying inside the existing scope clause of the `benchmark-harness`
capability.
