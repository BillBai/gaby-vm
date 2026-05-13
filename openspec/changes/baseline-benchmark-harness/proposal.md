## Why

The project's optimization plan — predecode cache → cached dispatch → repeated
execution (per `CLAUDE.md` and `docs/refs/gaby-vm-modification-sketch.md`) —
only has meaning relative to a measurable baseline. The methodology for
measuring the imported VIXL AArch64 simulator and comparing it to future
cached-dispatch builds is already sketched in
`docs/refs/baseline-benchmark-suite.md`, but nothing in the repository runs it.
The previous correctness change (`baseline-correctness-test`, archived
2026-05-12) explicitly deferred throughput measurement to this change. Without
a runnable harness, every later optimization — the cache, leaf tweaks, anything
else — would land without a measurement contract.

This change adds the runnable harness — workload, timing loop, reporting
format — so future work has a stable surface to land against.

## What Changes

- Add a new top-level `bench/` directory containing two runnable benchmark
  binaries that drive the imported `vixl::aarch64::Simulator` through
  `Simulator::RunFrom` over a fixed-size instruction workload: a baseline
  measurement binary (the real perf number) and a small smoke binary (a
  millisecond-scale end-to-end harness self-test).
- Add a `GABY_VM_BUILD_BENCHMARKS` CMake option to the top-level
  `CMakeLists.txt`, mirroring the shape of `GABY_VM_BUILD_TESTS` and
  `GABY_VM_BUILD_DEMOS`. Default to `${PROJECT_IS_TOP_LEVEL}`. Gate
  `add_subdirectory(bench)` on the option.
- Measurement protocol per `docs/refs/baseline-benchmark-suite.md`:
  - Single-threaded, release-build, process-level wall-time measurement.
  - Run a fixed workload of N instructions through `Simulator::RunFrom`, with
    a warm-up iteration discarded before the timed region.
  - Report instructions-per-second and ns-per-instruction.
- Two workloads, both shipped as committed instruction-bytes blobs (per the
  methodology doc's "build at test-data generation time, consume as bytes at
  runtime" pattern):
  - **`mixed`** — drawn from the methodology doc, a mixed integer + memory +
    branch loop generated offline via upstream VIXL's `BenchCodeGenerator`,
    representative of the Stretto/iOS target. The real perf workload. NOT
    per-instruction microbenchmarks.
  - **`smoke`** — a small (~32 instructions) straight-line workload generated
    offline via `llvm-mc` from a tiny `.s` file, with no upstream-VIXL
    dependency. Exercises the harness end-to-end in milliseconds; useful as a
    quick "did I break the harness?" check.
- Add `bench/README.md` describing:
  - How to configure and build with `GABY_VM_BUILD_BENCHMARKS=ON`.
  - How to invoke each benchmark binary.
  - What the output columns mean.
  - The manual procedure for cross-running the same workload shape against
    upstream VIXL's `bench-mixed-sim` for comparison (documented, not
    automated).

### Non-Goals (deferred to later changes)

- **Recording actual baseline numbers.** Numbers land in a separate report
  doc once the harness has been run on a stable reference host. This change
  ships the harness only.
- **CI / continuous-benchmarking automation, regression alerting, charts.**
  The harness is a developer-invoked tool in V1.
- **Comparison against native AArch64 hardware execution** (hardware perf
  counters, `perf record`, etc.).
- **Per-instruction-class microbenchmarks** (integer-only loop, memory-only
  loop, branch-only loop, NEON, SVE, atomics stress). The methodology doc
  lists these as P0/P1/P2; this change lands the harness shape and one
  representative workload. Additional workloads can be added later without
  re-litigating the harness contract.
- **Cache-on vs cache-off comparison.** The cache does not exist yet; that
  comparison belongs to the change that introduces the cache.
- **Multi-instance atomics stress** (the methodology doc's P0 correctness
  gate). It is a correctness check, not a throughput measurement, and belongs
  with the atomics/concurrency work.
- **Any change to the imported `src/` tree or to the `aarch64-simulator`
  capability.** The simulator stays as-is; the harness only consumes its
  existing public-ish surface (`Simulator`, `Decoder`, `RunFrom`, register
  accessors).

## Capabilities

### New Capabilities

- `benchmark-harness`: a developer-invoked harness that runs a fixed-size
  AArch64 instruction workload through the imported simulator, measures
  wall-clock throughput, and reports instructions-per-second and
  ns-per-instruction in a format aligned with the methodology in
  `docs/refs/baseline-benchmark-suite.md`. Owns the `bench/` source tree,
  the `GABY_VM_BUILD_BENCHMARKS` CMake option, the workload blob format,
  and the reporting contract.

### Modified Capabilities

*(none — `aarch64-simulator` is consumed by the harness but its requirements
do not change)*

## Impact

- **New top-level directory**: `bench/` with its own `CMakeLists.txt`,
  benchmark source(s), workload blob asset(s), and `README.md`.
- **Top-level `CMakeLists.txt`**: one new `option(GABY_VM_BUILD_BENCHMARKS ...)`
  line and one new `if(GABY_VM_BUILD_BENCHMARKS) add_subdirectory(bench)
  endif()` block. No other build changes.
- **No changes to `src/`** or to any imported VIXL file. No changes to
  `include/gaby_vm/`. No changes to `test/` or `demos/`.
- **Workload-generation dependency**: the workload blob is produced once
  using upstream VIXL's assembler (out-of-tree, per the methodology doc's
  guidance). The committed blob is plain data; the build does not depend on
  upstream VIXL at compile time. The generation procedure is documented in
  `bench/README.md` so the blob is reproducible.
- **CMake presets**: existing `dev-debug` / `dev-release` presets continue
  to work unchanged. Benchmarks are intended to be run from a release
  configuration; `bench/README.md` will say so explicitly.
- **No new public APIs.** Embedders are not exposed to anything new.
