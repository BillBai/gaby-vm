# continuous-integration Specification

## Purpose
TBD - created by archiving change add-github-ci. Update Purpose after archive.
## Requirements
### Requirement: The pull-request gate is deterministic

On every pull request and on every push to `main`, CI SHALL build the project,
run the full CTest suite, and run the cache/decoder parity check
(`bench_business --verify`). A pull request SHALL fail only on a deterministic
signal: a build failure, a CTest failure, or a parity mismatch. Timing-based
performance measurements SHALL NOT cause a pull request to fail.

#### Scenario: A clean change passes
- **WHEN** a pull request builds, the full CTest suite passes, and
  `bench_business --verify` reports agreement on all kernels
- **THEN** the CI gate passes

#### Scenario: A parity mismatch fails the gate
- **WHEN** `bench_business --verify` reports that the cache track and decoder
  track disagree on any kernel
- **THEN** the CI gate fails with a non-zero status

#### Scenario: A slow runner does not fail the gate
- **WHEN** the runner is slow and throughput numbers are lower than a previous
  run
- **THEN** the CI gate does not fail on that basis, because no timing threshold
  gates a pull request

### Requirement: Performance is tracked on main as a moving baseline

On push to `main` and on manual dispatch, CI SHALL run the full benchmark
harness on an arm64 macOS runner — the five business microkernels measured
three-way (native, decoder, cache) and the mixed VIXL workload — and publish the
numbers as a report artifact. This performance run SHALL NOT execute on pull
requests, and SHALL be report-only (it never fails the run).

#### Scenario: A push to main records the baseline
- **WHEN** a commit lands on `main`
- **THEN** the benchmark harness runs on the arm64 runner and uploads the
  result as an artifact

#### Scenario: Pull requests do not run the timing harness
- **WHEN** a pull request is opened or updated
- **THEN** no performance-timing job runs for that pull request

### Requirement: Binary size is reported with deltas and never gates

CI SHALL measure both the raw static-library size (`libgaby_vm.a`) and a
stripped linked-embedder footprint. On a pull request, CI SHALL report each
size relative to the latest available `main` baseline. Binary size SHALL be
report-only: a size increase SHALL NOT cause CI to fail.

#### Scenario: A base size is available
- **WHEN** a pull request runs and a `main` size baseline exists
- **THEN** the report shows both the absolute sizes and their deltas versus the
  baseline, and CI does not fail on size

#### Scenario: No base size yet
- **WHEN** a pull request runs and no `main` size baseline exists
- **THEN** the report shows the absolute sizes, states that no base is available
  for comparison, and CI still passes

### Requirement: CI logic is reproducible locally

All CI build, test, parity, size, and benchmark steps SHALL be implemented as
shell scripts under `ci/` that a developer can run locally to the same effect.
Workflow YAML SHALL only orchestrate these scripts (checkout, runner setup,
invoking scripts, publishing reports) and SHALL NOT embed build/test/bench/size
logic inline.

#### Scenario: A developer reproduces a CI step locally
- **WHEN** a developer runs a `ci/` script (e.g. `ci/ctest.sh`,
  `ci/size-report.sh`) on a local arm64 host
- **THEN** it performs the same step CI performs, without requiring the GitHub
  Actions environment

#### Scenario: A workflow step delegates to a script
- **WHEN** a workflow step performs a build/test/bench/size action
- **THEN** it invokes the corresponding `ci/` script rather than inlining the
  commands in the YAML

### Requirement: Reports are delivered to the job summary and pull request

Every CI run SHALL write a human-readable markdown report to the GitHub Actions
job summary. On a pull request, CI SHALL additionally post or update a single
sticky comment containing the test result, the parity result, and the size
deltas; repeated runs on the same pull request SHALL update that one comment
rather than adding new comments.

#### Scenario: Job summary always carries the report
- **WHEN** any CI run completes
- **THEN** its job summary contains the markdown report

#### Scenario: The PR comment is sticky
- **WHEN** a pull request is updated and CI re-runs
- **THEN** the existing CI comment is updated in place, and no duplicate CI
  comment is created

