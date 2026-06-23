## ADDED Requirements

### Requirement: Benchmark kernels are drivable through callable entry points

The benchmark harness SHALL expose each workload/kernel through a callable
entry point usable by a driver other than a bench executable's `main()` (for
example the iOS runner). The existing bench executables SHALL dispatch to the
same entry points, so the harness logic is not duplicated per driver.

#### Scenario: A non-CTest driver runs a kernel

- **WHEN** a driver other than a bench executable calls a kernel's entry point
- **THEN** it runs the same workload and produces the same metrics as the corresponding bench executable

#### Scenario: Bench executables dispatch to the shared entry

- **WHEN** a bench executable runs a workload
- **THEN** it dispatches to the shared entry point rather than reimplementing the workload body

### Requirement: A missing native baseline is reported explicitly

The harness SHALL report a missing native baseline explicitly rather than
silently omitting it: when it is asked to measure the native baseline but
executable memory for native execution is unavailable, it reports that the
native baseline was not run and continues reporting the available modes
(cache, decoder).

#### Scenario: Native baseline unavailable

- **WHEN** native execution memory cannot be obtained
- **THEN** the harness output contains an explicit indication that the native baseline did not run
- **AND** the cache and decoder measurements are still produced

#### Scenario: Native baseline available

- **WHEN** native execution memory is available
- **THEN** the native baseline is measured and reported as before
