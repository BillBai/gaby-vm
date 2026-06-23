## ADDED Requirements

### Requirement: The iOS Simulator test gate is deterministic

CI SHALL build the iOS runner for the iOS Simulator (arm64) and run its
correctness suites on every pull request and every push to `main` when the
runner is arm64. A failure of the iOS Simulator tests SHALL fail the gate. The
gate SHALL be implemented in a portable `ci/` script that the workflow invokes,
so a developer can reproduce it locally on an arm64 host.

#### Scenario: Passing iOS simulator tests pass the gate

- **WHEN** the iOS runner's simulator correctness suites all pass
- **THEN** the iOS gate passes

#### Scenario: A failing iOS simulator test fails the gate

- **WHEN** any iOS Simulator correctness test fails
- **THEN** the gate fails with a non-zero status

#### Scenario: The iOS gate reproduces locally

- **WHEN** a developer runs the iOS test `ci/` script on a local arm64 host
- **THEN** it performs the same simulator build and test that CI performs, without the GitHub Actions environment

### Requirement: A non-arm64 runner skips iOS explicitly

CI SHALL skip the iOS build/test and iOS benchmark steps when the runner cannot
run an arm64 iOS Simulator (for example an x86 runner), and SHALL report the
skip in the job summary and the sticky pull-request comment as an explicit
"skipped / not run" line. A skip SHALL exit successfully and SHALL NOT be
reported as a pass of the iOS tests.

#### Scenario: x86 runner skips without failing

- **WHEN** the runner is not arm64
- **THEN** the iOS test step exits successfully without running any iOS test
- **AND** the job summary and sticky comment contain an explicit line stating the iOS tests were skipped and not run

#### Scenario: A skip is not a pass

- **WHEN** the iOS step is skipped on a non-arm64 runner
- **THEN** the report does not present the iOS tests as having passed

#### Scenario: arm64 runner runs rather than skips

- **WHEN** the runner is arm64 with an available iOS Simulator
- **THEN** the iOS step runs the simulator tests instead of skipping

### Requirement: iOS performance is reported on main and never gates

On push to `main` and on manual dispatch, when the runner is arm64, CI SHALL
run the iOS benchmark on the iOS Simulator and include the numbers in the
report artifact. The iOS performance run SHALL NOT execute on pull requests and
SHALL be report-only: an iOS timing change SHALL NOT fail the run.

#### Scenario: A push to main records iOS numbers

- **WHEN** a commit lands on `main` and the runner is arm64
- **THEN** the iOS benchmark runs on the simulator and its numbers are included in the report artifact

#### Scenario: Pull requests do not run the iOS timing job

- **WHEN** a pull request is opened or updated
- **THEN** no iOS performance-timing run executes for that pull request

#### Scenario: An iOS timing regression does not fail the run

- **WHEN** an iOS benchmark number is worse than a previous run
- **THEN** the run does not fail on that basis
