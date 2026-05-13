## Context

The proposal establishes WHY we want a benchmark harness: every future
optimization (the predecode cache, leaf tweaks, anything else) needs a
measurement contract to land against, and the methodology in
`docs/refs/baseline-benchmark-suite.md` is currently un-runnable.

This document records HOW the harness is built — the decisions whose rationale
would otherwise be relitigated when tasks.md is drafted or when reviewers ask
"why this and not that?".

Current state:

- The imported simulator already lives at `src/aarch64/simulator-aarch64.{h,cc}`,
  exposed via the headers in `src/aarch64/` and the include surface used by
  `test/simulator_smoke.cc` and `test/simulator_correctness.cc`.
- `test/CMakeLists.txt` establishes the working pattern for consuming the
  imported simulator: `PRIVATE` include of `${PROJECT_SOURCE_DIR}/src`, the
  `VIXL_INCLUDE_TARGET_A64` / `VIXL_INCLUDE_SIMULATOR_AARCH64` defines, and the
  conditional `VIXL_DEBUG` for debug builds.
- `CMakeLists.txt:12-13` defines `GABY_VM_BUILD_TESTS` and `GABY_VM_BUILD_DEMOS`
  with `${PROJECT_IS_TOP_LEVEL}` as default. The new `GABY_VM_BUILD_BENCHMARKS`
  option mirrors that shape exactly.
- Upstream VIXL ships `benchmarks/aarch64/bench-mixed-sim.cc`, the canonical
  shape we need to mirror so our numbers are directly comparable. Its
  `BenchCodeGenerator` is in Tier 0 (assembler family) and is therefore NOT
  imported — we generate the equivalent buffer offline.

Constraints carried in from `CLAUDE.md`:

- No JIT, no RWX memory, no executable memory allocation. The workload is
  *data* the simulator interprets, not host-executable code. Treating the
  workload as plain bytes also makes iOS embedding trivial later.
- Portability across iOS, macOS, POSIX-like systems. The harness uses only
  `<chrono>` and standard library facilities; no platform-specific clocks.

## Goals / Non-Goals

**Goals:**

- One runnable benchmark binary that drives the imported simulator over a
  fixed-shape mixed workload and reports throughput + per-instruction cost.
- Workload-generation procedure that mirrors upstream `bench-mixed-sim` shape
  closely enough that the same workload (in spirit) can be cross-run against
  upstream VIXL for direct comparison.
- Build integration that follows the existing `GABY_VM_BUILD_*` option pattern
  without touching `src/` or imported VIXL files.
- A reporting format that a human or a `grep`/`awk` pipeline can read cleanly.

**Non-Goals:**

- Multi-workload selection by flag (a `--workload <name>` registry),
  microbenchmarks per instruction class, additional workload kinds beyond
  the two shipped here — defer to later changes once the harness contract
  is proven on the V1 pair.
- Statistical aggregation (median / IQR over N runs) — the harness produces
  one observation per invocation; if a developer wants 10 runs, they invoke
  the harness 10 times. Aggregation belongs in the eventual report-doc change.
- Cache-on/cache-off toggling — the cache does not exist yet; the harness has
  no awareness of a cache.
- Multi-instance atomics stress — that's a correctness test, not a throughput
  measurement, and belongs with the atomics/concurrency work.
- CI integration, regression alerting, charts — out per the proposal.

## Decisions

### Decision 1: Workload is a committed C++ header containing the instruction bytes

**Choice.** Each workload ships as a generated C++ header under
`bench/workloads/`, with a uniform schema so the harness can consume any
workload without per-workload branching. V1 has two headers:
`mixed_workload_data.h` and `smoke_workload_data.h`. The schema (using
`mixed` as the example name; `smoke` mirrors it):

- `inline constexpr uint32_t kMixedWorkloadInstructions[]` — the instruction
  word stream emitted by the generator.
- `inline constexpr size_t kMixedWorkloadStaticWordCount` — convenience
  static count (`std::size(...)`). This is the number of 4-byte instruction
  words physically present in the buffer.
- `inline constexpr uint64_t kMixedWorkloadDynamicInstructionsPerIteration` —
  the number of instructions actually executed by a single
  `Simulator::RunFrom(start)` call over this buffer. Captured offline (see
  Decision 2). Required for `mixed` because `BenchCodeGenerator`'s output
  contains taken branches, conditional fall-throughs, and nested call/return
  sequences, so dynamic count ≠ static word count. For `smoke` the dynamic
  count equals the static count by construction (straight-line code, no
  branches taken across the body); we still emit the field so the harness
  code path is identical for both workloads.
- `inline constexpr const char kMixedWorkloadGeneratorTag[]` — provenance
  string. For `mixed`: upstream VIXL commit hash + RNG seed + buffer-size
  constant. For `smoke`: `llvm-mc` version + source `.s` digest.

The harness `#include`s the relevant header, copies the array into a
simulator-visible buffer, and calls `Simulator::RunFrom` on the buffer start.
Throughput uses the recorded `…DynamicInstructionsPerIteration`, not the
static word count.

**Why.** Maximizes simplicity and reproducibility:

- No file I/O, no asset-discovery code, no path resolution. The header is part
  of the translation unit; the bytes are linked in.
- Diffable: a future change to the workload shows up in `git log` as a normal
  source change with explicit provenance in the header.
- Trivially portable: works the same on iOS, macOS, and Linux without
  filesystem assumptions.
- The size is bounded and modest (256 KiB = 64 Ki instructions × 4 B). It fits
  cleanly in a header without bloating compile times.

**Alternatives considered.**

- *Binary blob file loaded at runtime.* More flexible if we eventually ship
  many workloads, but adds path-resolution code, ties tests to a working
  directory or install prefix, and complicates iOS embedding. Defer until V1
  outgrows a single workload.
- *Link upstream VIXL into the bench binary and generate at startup.* Pulls
  Tier 0 (assembler, macro-assembler, code buffer) into the build, which the
  project policy explicitly forbids. Hard no.
- *Hand-write the instruction stream as inline `uint32_t` literals.* Possible
  for tiny workloads but does not produce the upstream `bench-mixed-sim`
  mix-and-density shape the methodology doc requires.

### Decision 2: Workload generation is offline, documented, not scripted in-tree

Each workload has its own generation path. Both produce a committed C++
header following the Decision 1 schema; neither is invoked by the build.

#### Mixed workload — via upstream VIXL `BenchCodeGenerator`

The header is regenerated by running a small standalone C++ program (kept
*outside* this repository, in a developer's `../vixl` checkout or a scratch
directory) that:

1. Constructs a `MacroAssembler` with `CPUFeatures::All()` and a fixed RNG
   seed (default 0; the seed is recorded in the provenance tag).
2. Drives `BenchCodeGenerator::Generate(buffer_size)` over a 256 KiB buffer.
   The generator emits its own prologue (`Push(lr, x29)`, frame setup,
   stack scratch) and its own epilogue (`Pop(x29, lr); Ret()`), so the
   buffer is self-contained — no extra epilogue is appended.
3. Calls `masm.FinalizeCode()` and reads the instruction bytes from the
   macro-assembler's code buffer.
4. Constructs a `Simulator`, calls `simulator.SetCPUFeatures(CPUFeatures::All())`,
   enables the simulator's existing trace mechanism via
   `set_trace_parameters` (configured to count rather than print), and runs
   `simulator.RunFrom(start)` once. The trace hook fires per executed
   instruction; we sum the firings to get the dynamic instruction count.
   *(We chose the `set_trace_parameters` hook over writing a custom
   `DecoderVisitor`: the hook already exists in upstream and "configure with
   a no-op formatter, count callbacks" is a few lines, while a visitor would
   reproduce machinery that's already wired up.)*
5. Emits a C++ header to stdout containing the byte array, the static word
   count, the recorded dynamic instruction count, and the provenance tag.

The exit convention is the upstream simulator's existing `LR ==
kEndOfSimAddress` mechanism: `Simulator::ResetRegisters()` sets `LR` to
`kEndOfSimAddress` (which is `NULL` — see `simulator-aarch64.cc:58,720`), the
workload's prologue saves `LR` and its epilogue restores it, and the final
`Ret()` branches the simulated PC to `kEndOfSimAddress`, at which point
`RunFrom` returns. Nothing in the buffer "branches to NULL"; the sentinel is
reached only via `RET`. The Gaby-VM harness must therefore call
`simulator.WriteLr(Simulator::kEndOfSimAddress)` before each `RunFrom` if it
cannot prove the prior iteration restored `LR` cleanly (in practice
`BenchCodeGenerator`'s epilogue does restore it, but a defensive write is
cheap insurance for future workloads).

#### Smoke workload — via `llvm-mc`

The smoke workload is intentionally trivial:

1. A short `.s` file (~30 lines including prologue/epilogue) committed under
   `bench/workloads/smoke_workload.s`. Body is straight-line ALU
   instructions (e.g., `Add x0, x0, 1` × N) wrapped by the same
   `Push(lr, x29) … Pop(x29, lr); Ret()` convention so the LR-based exit
   works identically to the mixed workload.
2. `llvm-mc -arch=aarch64 -filetype=obj` assembles the file; `llvm-objcopy
   -O binary` extracts the raw text section. A trivial shell pipeline in
   `bench/README.md` converts the bytes into `constexpr uint32_t[]` literals
   for the header.
3. No simulator run is needed for the dynamic count: by construction (no
   branches), static word count and dynamic instruction count are equal.
   The header still records both, for schema uniformity.

This matches the pre-recorded "authorship-time external tools OK" pattern
(`feedback_authorship_time_tools.md`): `llvm-mc` runs at workload-authoring
time, its output is hand-copied into source, and there is zero build-time or
runtime dependency on it.

#### Shared

`bench/README.md` ships the exact source for both generation procedures plus
the toolchain invocations needed (`scons simulator=aarch64 mode=release` for
mixed; `llvm-mc` + `llvm-objcopy` for smoke). Generated headers are committed
and treated as source.

**Why.** Keeps the build hermetic — there is no build-time dependency on
upstream VIXL or LLVM. Regeneration is a developer ceremony (rare; only when
the methodology or smoke source changes), and the procedure is reproducible
because seeds and tool versions are recorded in each header's provenance
string.

**Alternatives considered.**

- *Commit a generator script in `bench/scripts/` that wraps the upstream
  build.* Implies upstream VIXL is checked out at a known relative path,
  which is currently a documentation convention (`../vixl`) but not a
  build-system contract. Adds fragility for thin gain.
- *Generate at CMake configure time.* Adds a build-time dependency on upstream
  VIXL and slows clean configures. Worse: it conflates "the workload" with
  "whatever upstream produced yesterday."
- *Custom `DecoderVisitor` for instruction counting (mixed workload).*
  Functionally equivalent to the `set_trace_parameters` hook chosen above —
  picked the hook because it's already wired through the simulator's trace
  path and needs less new code.
- *Hand-coded `uint32_t` literals for smoke.* Possible but error-prone for
  even ~32 instructions; a single typo silently corrupts the workload.
  `llvm-mc` removes the encoding step from the human.

### Decision 3: Time-bounded loop, count iterations — mirror upstream `bench-mixed-sim`

**Choice.** The steady-state loop is:

```cpp
// Warm-up — discarded.
simulator.RunFrom(start);

auto t0 = std::chrono::steady_clock::now();
uint64_t iterations = 0;
do {
  simulator.RunFrom(start);
  ++iterations;
} while (std::chrono::steady_clock::now() - t0 < target_duration);
auto t1 = std::chrono::steady_clock::now();
```

`target_duration` defaults to **5 seconds**, matching upstream's
`BenchCLI::kDefaultRunTime = 5` (`bench-utils.h:161`); configurable via
`--seconds <float>` on the command line.

The **primary reported metric is iterations per second**
(`iterations / elapsed_seconds`), identical to upstream's
`BenchCLI::PrintResults` (`bench-utils.h:121-132`). This makes cross-comparison
against upstream's `bench-mixed-sim` direct: same workload bytes, same loop
shape, same headline metric.

Total instructions executed is computed as
`iterations × kMixedWorkloadDynamicInstructionsPerIteration`, using the
**dynamic** count recorded offline (see Decision 2). It is NOT computed from
the static word count `kMixedWorkloadStaticWordCount`: the upstream
`BenchCodeGenerator` mixed workload contains taken branches,
conditional fall-throughs, and nested call/return blocks, so the number of
emitted instruction words ≠ the number of instructions executed per
`RunFrom` call. Dividing by the static count would publish a wrong
ns/insn for this exact workload. ns/insn is therefore a derived secondary
metric, only valid when a dynamic count has been recorded for the workload.

**Why.** This is byte-for-byte the shape of upstream's
`benchmarks/aarch64/bench-mixed-sim.cc`. Matching the upstream loop is the
single largest factor in making the cross-comparison defensible: any
divergence in loop shape (e.g., timing per-call instead of per-batch)
introduces a noise variable that has to be controlled for separately.

**Why not "run the workload exactly once and report its time"?** A single
`RunFrom` over a 256 KiB buffer on a reasonable host completes in
sub-millisecond range. Wall-clock resolution and noise dominate. A few
seconds' worth of iterations is the de facto industry choice for stable
single-shot benchmarks; upstream uses 5 s, so we adopt that.

**Alternatives considered.**

- *Fixed iteration count, not duration.* Simpler invariant ("did exactly N
  iterations") but produces unstable wall-clock numbers across hosts of
  different speed. Upstream chose duration-bounded for good reason.
- *Per-call timing with statistical aggregation inside the harness.* Tempting
  but cross-cuts with the explicit out-of-scope "no statistical aggregation
  in V1." Re-aggregate at the report-doc layer.

### Decision 4: Timing primitive is `std::chrono::steady_clock`

**Choice.** `std::chrono::steady_clock` for the timing window. Output durations
are converted to seconds (double) at report time.

**Why.**

- Monotonic across all target platforms (iOS, macOS, Linux, Android).
- Resolution on every modern platform is sub-microsecond — many orders of
  magnitude finer than the multi-second measurement window. Resolution is not
  a bottleneck.
- No platform conditionals required.

**Alternatives considered.**

- *Replicating upstream `BenchTimer`.* That helper lives in Tier 0 (upstream
  `benchmarks/aarch64/bench-utils.{h,cc}`) and is not imported. Re-implementing
  it ourselves is gratuitous when `<chrono>` is sufficient.
- *Platform-specific cycle counters (`rdtsc`, `mach_absolute_time`,
  `clock_gettime(CLOCK_MONOTONIC_RAW)`).* Better resolution, but at a
  measurement window of seconds the marginal value is zero, and the
  portability cost is positive.

### Decision 5: One binary per workload in V1

**Choice.** V1 produces two binaries, each hard-coded to one committed
workload header:

| Target / output | Source file | Workload header |
|---|---|---|
| `bench_baseline` | `bench/baseline.cc` | `bench/workloads/mixed_workload_data.h` |
| `bench_smoke` | `bench/smoke.cc` | `bench/workloads/smoke_workload_data.h` |

`bench_baseline` is the real perf measurement (seconds-long run, mixed
workload). `bench_smoke` is the harness self-test (milliseconds-long run,
straight-line workload — useful when you want to verify the harness pipeline
itself without burning the full mixed run).

Both binaries share the same timing-loop, exit-convention, and reporting
code (factored into a small `bench/runner.{h,cc}`); only the included
workload header differs.

**Why.** Defers the "workload selection" UX (a `--workload <name>` flag, a
registry, command-line dispatch) until there's a reason for it. N=2 doesn't
justify a registry — duplicating ~20 lines of `main()` per binary is cheaper
than designing a selection surface. Adding a third workload later either
copies one more `.cc` or, if N=3 makes the duplication painful, triggers a
small refactor with concrete data to inform the design.

**Alternatives considered.**

- *Single `bench` binary with `--workload {mixed,smoke}` from day one.*
  Reasonable, but the registry plumbing only earns its keep at N≥3 in
  practice. Defer.
- *Two binaries but a single `bench_main.cc` driven by build-time defines.*
  Saves ~20 lines per binary, costs clarity (now reading `main` requires
  knowing the build flags). Not worth it at this scale.

### Decision 6: Output is plain-text key/value lines

**Choice.** The benchmark writes lines like:

```text
workload: mixed
workload_generator_tag: vixl@<commit>; seed=0x0; buffer_bytes=262144
static_words_in_buffer: 65536
dynamic_instructions_per_iteration: 71428
iterations: 7421
total_dynamic_instructions: 530000588
elapsed_seconds: 5.0002
iterations_per_second: 1484.18                 # primary; matches upstream
throughput_insn_per_sec: 105995517.8           # derived from dynamic count
ns_per_instruction: 9.434                      # derived from dynamic count
```

(Numbers are illustrative — real numbers land in the follow-up report doc.)

One observation per invocation. Stable, parseable, no JSON dependency, easy to
diff across runs. The "primary" iterations-per-second line is the metric to
compare directly against upstream's `bench-mixed-sim`; the derived ns/insn
and insn/sec lines depend on the recorded dynamic count and are only as
trustworthy as the offline measurement that produced it.

**Why.** Matches upstream's reporting style (iterations/sec) and the
methodology doc's preferred metric language (ns/insn), and is
shell-pipeline friendly. JSON is deferred until something downstream actually
consumes it.

**Alternatives considered.**

- *JSON output by default.* Adds a dependency or a hand-rolled serializer for
  no immediate consumer. Defer.
- *A single line of numbers for spreadsheet pasting.* Hostile to humans;
  labeled key/value pairs are no harder to parse and far easier to read.

### Decision 7: CMake target wiring mirrors `test/CMakeLists.txt`

**Choice.** `bench/CMakeLists.txt`:

- Defines an `OBJECT` library `bench_runner` from the shared
  `runner.{h,cc}` (timing loop, exit-convention reset, reporting).
- Defines two executables: `bench_baseline` (from `baseline.cc`) and
  `bench_smoke` (from `smoke.cc`). Each links the `bench_runner` object
  library plus the same simulator surface `test/` consumes.
- Applies the same include + compile-definition pattern as
  `test/CMakeLists.txt` to all three targets:
  `target_include_directories(... PRIVATE ${PROJECT_SOURCE_DIR}/src)`
  and `target_compile_definitions(... PRIVATE VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64 $<$<CONFIG:Debug>:VIXL_DEBUG>)`.
- Does NOT register either binary with `add_test(...)` — benchmarks are
  developer-invoked, not part of CTest. (`bench_smoke` is fast enough that
  a future change could add it to a CI smoke pipeline, but that's not part
  of this change.)

Top-level `CMakeLists.txt` gets one new `option()` line and one new
`if(GABY_VM_BUILD_BENCHMARKS) add_subdirectory(bench) endif()` block.

**Why.** Re-uses a working pattern verbatim and avoids surprises. The
`OBJECT` library factoring keeps the shared timing/reporting code in one
place without forcing it into `src/` (which is reserved for imported VIXL).
Keeping benchmarks out of CTest preserves the convention that `ctest` is
for correctness, not performance.

## Risks / Trade-offs

- **[Workload-blob drift vs the imported simulator]** → If the workload header
  is regenerated against a newer upstream VIXL than the imported Tier-1/2/3
  baseline, the bytes may include instructions or encodings the imported
  simulator does not yet support. Mitigation: the header's
  `kMixedWorkloadGeneratorTag` records the upstream commit hash; `bench/README.md`
  states the expectation that the tag matches (or is older than) the imported
  baseline. The harness can also `VIXL_CHECK` that the simulator's
  `CPUFeatures` permits everything in the buffer, but the cheapest defense is
  documentation.

- **[Wall-clock noise on a casual host]** → Numbers will vary across runs on
  a laptop with turbo, a busy desktop, or a noisy CI runner. **This is
  acceptable.** V1 targets order-of-magnitude correctness, not
  publication-grade measurements: the harness exists to validate that the
  predecode-cache optimization moves the number in the right direction by a
  meaningful factor, not to publish ns/insn to two-decimal precision.
  Mitigation: `bench/README.md` notes only the bare minimum ("don't run on
  battery, prefer a mostly-idle machine") and explicitly does not prescribe
  core pinning, turbo control, or governor tweaks. If a future change needs
  tighter numbers (a regression-gating CI, a published report), that change
  is the right place to introduce a noise-control protocol — not V1.

- **[`RunFrom` call-site overhead amortized across the buffer]** → Each loop
  iteration includes the call-site overhead of `RunFrom`. With ~64 Ki
  instructions per iteration that overhead is in the noise floor (≪1% of an
  iteration's runtime). If a future workload is much smaller, the overhead
  would become visible. Mitigation: workload size is a property of the
  workload header; reviewers of a future small-workload change must consider
  this. Document in README.

- **[Header-file blob locks workload size to compile-time constant]** →
  Increasing workload size requires regenerating the header and rebuilding.
  This is fine for V1 (one workload, fixed shape). It would be friction if
  workload size became a tuning knob. Mitigation: when the second workload
  lands, evaluate whether to keep header-blob or switch to a file-loaded
  binary asset. Don't pre-design for it now.

- **[Default `--seconds 5` may be too short on a slow host or too long on a
  fast one]** → If a host takes 50 ms per iteration, five seconds is only
  ~100 iterations and noise is high. If a host runs at 0.5 ms/iter, five
  seconds is ~10,000 iterations — fine, just a bit wasteful. Mitigation: it's
  a CLI flag, not a hardcoded constant. README documents tuning guidance and
  matches upstream's `BenchCLI::kDefaultRunTime`.

- **[Recorded dynamic count drifts from actual execution]** → The dynamic
  count committed in the workload header is captured once offline. If the
  imported simulator's behavior diverges from upstream's at runtime (a marker
  edit that changes a leaf, a future bug), the recorded count and the actual
  number of dispatched instructions can desync. The derived ns/insn would
  then be wrong even though iterations/sec stays right. Mitigation: the
  primary reported metric is iterations/sec — directly observable, not
  derived. ns/insn is labeled "derived" in the output to flag its dependence
  on the recorded count. A future workload regeneration also re-records the
  count, so the drift is bounded to whatever has shifted since the last
  regeneration.

- **[Two workloads share the same harness, but their roles differ]** →
  `bench_baseline` is the measurement; `bench_smoke` is the self-test. If a
  reader confuses them (e.g., quotes `bench_smoke` numbers as the project
  baseline), the published number is meaningless. Mitigation:
  `specs/benchmark-harness/spec.md` (next artifact) states the harness
  contract in workload-agnostic terms (input shape, output format, exit
  convention) so it applies equally to both; `bench/README.md` is explicit
  about which binary you cite for what.

## Migration Plan

Not applicable. The harness is a new, additive surface with no existing
consumers; there is no rollback strategy beyond "remove the `bench/` directory
and the CMake option." No deprecation of existing tools.

## Open Questions

All three V1 open questions were resolved during design review:

1. **Instruction counting in the offline mixed-workload generator.**
   *Resolved:* use the simulator's existing `set_trace_parameters` hook
   (counting callbacks, no printing), not a custom `DecoderVisitor`.
   Captured in Decision 2.

2. **256 KiB / 64 Ki buffer size for the mixed workload.**
   *Resolved:* keep upstream's 256 KiB unchanged in V1. iOS-specific sizing
   is deferred to whatever change first targets an iOS embedding host;
   there is no V1 evidence that a different size produces more useful
   numbers.

3. **Ship a smoke workload alongside the mixed workload?**
   *Resolved: yes.* Promoted into scope as a second binary (`bench_smoke`)
   driven by a straight-line `llvm-mc`-generated workload. Captured in
   Decisions 1, 2, 5, and 7. The proposal's "What Changes" lists it
   explicitly.
