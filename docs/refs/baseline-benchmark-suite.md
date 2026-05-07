# Baseline Benchmark Suite

> Methodology for measuring the upstream VIXL AArch64 simulator and
> comparing it to Gaby-VM's cached-dispatch path. **This document
> captures the methodology, not results** — actual baseline numbers
> land in a follow-up report after we run them on a stable host.
>
> Citations are paths inside `../vixl/`. The architectural references
> live in
> [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md)
> and
> [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md);
> the cache design lives in
> [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md).

## Compiling upstream VIXL with the simulator

VIXL ships with SCons. From `../vixl/`:

```bash
# Library (libvixl.a) with the AArch64 simulator compiled in.
scons simulator=aarch64 mode=release -j$(nproc)

# Library + AArch64 examples.
scons simulator=aarch64 mode=release aarch64_examples

# Library + AArch64 simulator benchmarks (incl. bench-mixed-sim).
scons simulator=aarch64 mode=release aarch64_benchmarks

# Library + test-runner binary.
scons simulator=aarch64 mode=release tests

# Everything.
scons simulator=aarch64 mode=release all
```

The `simulator=aarch64` option auto-defaults to "on if the host is not
AArch64, else off" — see the SConstruct comment near the
`vars_default_handlers` table at `SConstruct:~290`. On an AArch64
host, set it explicitly: the upstream simulator can run on an AArch64
host, but cross-checking against native execution is also useful.

Defines that get applied automatically:

- `VIXL_INCLUDE_TARGET_A64` (target=a64).
- `VIXL_INCLUDE_SIMULATOR_AARCH64` (simulator=aarch64).
- `VIXL_DEBUG` (mode=debug only).
- `VIXL_CODE_BUFFER_MMAP` or `VIXL_CODE_BUFFER_MALLOC` (default
  `mmap` on Linux, `malloc` elsewhere; auto-handler in SConstruct).

Outputs land under `obj/release/...` (or `obj/debug/...`):

- `libvixl.a` — static library.
- `obj/.../benchmarks/aarch64/bench-mixed-sim` — the simulator
  benchmark (built when `aarch64_benchmarks` is targeted).
- `obj/.../test/test-runner` — the test runner (built when `tests`
  is targeted).

## Running upstream tests

```bash
# Build the test runner and the donkey simulator-trace generator.
scons simulator=aarch64 mode=release tests

# List all test names.
obj/release/test/test-runner --list

# Run all simulator tests (regex-filtered).
obj/release/test/test-runner --run_test 'AARCH64_SIM_.*'

# Run a specific test by name.
obj/release/test/test-runner --run_test 'AARCH64_SIM_basic_operations'
```

For day-to-day driving, `tools/test.py` orchestrates a debug+release
matrix and runs lints. To skip clang-format/clang-tidy (which require
specific tool versions installed):

```bash
tools/test.py --noclang-format --noclang-tidy
```

## Benchmark cases we want for Gaby-VM

Upstream has only one simulator-targeting benchmark,
`benchmarks/aarch64/bench-mixed-sim.cc`:

```cpp
// from bench-mixed-sim.cc
const size_t buffer_size = 256 * KBytes;
MacroAssembler masm(buffer_size);
masm.SetCPUFeatures(CPUFeatures::All());
BenchCodeGenerator generator(&masm);

masm.Reset();
generator.Generate(buffer_size);
masm.FinalizeCode();

const Instruction* start =
    masm.GetBuffer()->GetStartAddress<const Instruction*>();

Decoder decoder;
Simulator simulator(&decoder);
simulator.SetCPUFeatures(CPUFeatures::All());

BenchTimer timer;

size_t iterations = 0;
do {
  simulator.RunFrom(start);
  iterations++;
} while (!timer.HasRunFor(cli.GetRunTimeInSeconds()));

cli.PrintResults(iterations, timer.GetElapsedSeconds());
```

The harness uses `BenchCodeGenerator` (in `bench-utils.{h,cc}`) — a
deterministically-seeded generator that fills the supplied buffer with
a representative mix of integer ALU, memory, branches, and FP/NEON.
The generated code includes a prologue and epilogue so the simulator
exits cleanly via the `kEndOfSimAddress` convention.

Gaby-VM's mirror benchmarks should reuse this *shape* but, because we
do not import the macro-assembler family in V1, the harness will:

1. Build the input code with upstream VIXL's assembler at *test-data
   generation time*. Dump the resulting instruction bytes into a header
   or binary blob committed to the Gaby-VM repo.
2. Read the blob into a host buffer at runtime in the Gaby-VM
   benchmark.
3. Hand the buffer's start to Gaby-VM's simulator as the entry point.

This decouples the runtime benchmark from any code-generation
dependency.

### Per-case benchmark list

| Case | What it stresses | Priority |
|------|------------------|----------|
| `bench-mixed-sim` mirror | mixed integer / memory / branch / FP — direct apples-to-apples vs upstream | P0 |
| Tight integer loop (sum-of-N) | dispatch overhead alone (ALU is cheap) | P0 |
| Memory-heavy loop (load/store unsigned offset over a small buffer) | `MemRead<T>` + `AddressUntag` cost | P0 |
| Branchy loop (interleaved conditional branches) | post-branch PC lookup is hot — exercises the predecode array index path | P0 |
| NEON arithmetic loop | vector throughput; cache must not regress vector code | P1 |
| SVE loop | same as NEON but for SVE forms | P2 (deferred) |
| **Multi-instance atomics stress** (correctness gate, not throughput) | exclusive-monitor and LSE atomic correctness across host threads | **P0 correctness** |

The multi-instance stress benchmark is the critical correctness
gate — see "Multi-instance atomics stress" below.

## Metrics to record

For every benchmark case, capture:

- **Throughput.** Simulated instructions per second:
  `(instructions_emitted_per_iter × iterations) / elapsed_seconds`. Use
  steady-state (warmup discarded) median over N=10 runs; report
  median, IQR, and `(max - min) / median`.
- **Per-instruction cost** (ns/insn). Direct inverse of throughput;
  more useful for comparing dispatch designs.
- **Decode-vs-leaf split.** Sampled profiling (e.g. `perf record -g`)
  on the hot benchmark case. We want to know what fraction of cycles
  is dispatch vs leaf semantics — this tells us when the cache has
  pushed dispatch overhead below the noise floor.
- **Cache-hit counter** (Gaby-VM only). On cached builds, count hits
  vs the out-of-range fallback; near-100% hit rate is expected.
- **Predecode cost** (Gaby-VM only). Time `RegisterCodeRange` for the
  benchmark blob; report as "ms to predecode N instructions". Useful
  for cold-start sensitivity in embeddings that load code dynamically.
- **Build / host context.** Record alongside numbers:
  - Compiler + version (e.g. clang 18, gcc 14).
  - Optimization level (`-O3` for release; the flag is set by SCons
    automatically).
  - Host CPU model.
  - Host kernel / OS version.
  - CPU governor, frequency lock state, turbo on/off, core pinning
    (`taskset -c 2`).

## How we compare upstream VIXL vs Gaby-VM

Three comparisons matter:

### Upstream VIXL baseline

Run upstream `bench-mixed-sim` with `simulator=aarch64 mode=release`
on the same host configuration as Gaby-VM, with the same generated
code shape if practical (i.e. seed `BenchCodeGenerator` identically
or use the dumped blob as input to a small wrapper). Record
throughput and ns/insn.

This is the *floor* Gaby-VM must not regress against in V1 (cache off
or out-of-range fallback path) and the headroom Gaby-VM should
exceed (cache on, hot path).

### Gaby-VM cache-on vs cache-off

Provide either a build flag or a runtime toggle that disables cache
hits and forces the slow path on every instruction. Run the same
benchmark twice; compute speedup as
`throughput(cache_on) / throughput(cache_off)`.

Equality of resulting register/memory state across the two paths is
a **correctness gate**: the benchmark harness should `VIXL_CHECK`
that both paths produce identical state at the end of each iteration
(or at instrumented checkpoints), not just identical timings. See
"Self-test" in
[`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md).

### Reporting

Per benchmark and per host configuration:

- Median ns/insn and median throughput, with IQR.
- Speedup percentage with confidence interval over the 10 runs.
- A separate column for "predecode cost (ms)" — this is *not*
  included in the steady-state ns/insn but is reported alongside
  for embedders who care about cold-start latency.

Sample row:

```
case: bench-mixed-sim mirror
  upstream-VIXL    : median 47.3 ns/insn  (IQR 1.2 ns)
  gaby-vm cache off: median 49.1 ns/insn  (IQR 1.5 ns)   [+3.8% slower]
  gaby-vm cache on : median 18.6 ns/insn  (IQR 0.4 ns)   [-60.7% / 2.54x]
  predecode cost   : 8.7 ms for 65,536 instructions
```

(Numbers are illustrative.)

## Predecode registration in the benchmark loop

Because the predecode buffer is pre-allocated and populated up-front
(no lazy fill, no invalidation — see the [modification
sketch](./gaby-vm-modification-sketch.md)),
`RegisterCodeRange(start, size)` runs **once per blob, outside the
timed steady-state region**. The benchmark must:

- Time the registration call separately (`BenchTimer` around it).
  Report it as `predecode cost (ms) for N instructions`.
- Keep the steady-state loop confined to `simulator.RunFrom(start)`
  only, identical in structure to upstream `bench-mixed-sim`, so the
  iterations/sec numbers are directly comparable.
- Run a warm-up iteration before the timed loop so any first-touch
  cache effects in the host CPU don't skew the first iteration.

## Multi-instance atomics stress

This is a correctness gate, not primarily a throughput measurement.

### Setup

Spawn N host threads (default N = number of online cores). Each
thread:

1. Allocates its own embedder-provided stack buffer.
2. Constructs its own Simulator instance.
3. Registers the same shared code range (immutable, populated once
   before any thread starts).
4. Runs `simulator.RunFrom(test_program_start)` to completion.

The test program is a tight loop in guest assembly that increments a
shared counter via atomic operations. Two variants:

- **LDXR/STXR variant.** Classic load-exclusive / store-exclusive
  loop; retry on STXR failure. Exercises the per-instance exclusive
  monitor and the host CAS used to emulate STXR.
- **LSE variant.** `LDADD x0, [counter]` once per iteration.
  Exercises the LSE atomic dispatch directly.

Each thread runs `M` iterations. The counter starts at 0; expected
final value is `N × M`.

### Pass criteria

- Final counter value equals `N × M` (no lost updates, no double
  counts).
- All threads' guest registers and stacks contain expected values
  (each thread's local state should be unaffected by other threads).
- No data races detected by ThreadSanitizer in a TSan-instrumented
  build.

### Failure modes the benchmark catches

- Per-thread monitor state escaping into a process-wide global —
  symptoms: STXR succeeds when it shouldn't, lost updates.
- LSE atomic implemented as a non-atomic load/store — symptoms:
  counter undercounts roughly by `N - 1` per iteration on contended
  cases.
- 16-byte pair atomic (CASP, LDXP/STXP) emulated incorrectly on
  hosts without `cmpxchg16b` — symptoms: torn reads or lost updates
  on pair forms.
- Forgotten barrier (DMB ISH) — symptoms: spurious ordering
  inversions visible to a TSan run.

### Throughput as a secondary signal

The atomic stress benchmark also produces a throughput number
(updates/sec). Useful as a regression sanity check — a sudden 10× drop
between commits indicates an atomics implementation regression even
when correctness is unchanged.

## Practical notes

- **Pin to a single core** for single-instance benchmarks
  (`taskset -c 2 ./bench-...`). Disable turbo if available
  (`/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` →
  `performance`; turbo via vendor tooling).
- **Run in a quiet shell.** Close browsers, syncing daemons, and
  anything that might steal cache.
- **Keep the buffer size and total instruction count constant across
  versions.** Otherwise iteration counts are not comparable; the
  bench-mixed-sim harness uses a 256 KiB buffer by default — keep it.
- **Run on macOS and Linux as a minimum.** Defer iOS until a usable
  embedding target exists; the constraints in
  [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
  are designed to make iOS embedding eventually trivial, but the
  benchmark harness has no iOS path in V1.
- **Don't run on a laptop on battery.** Frequency scaling makes the
  numbers noise-dominated.

## Where to read next

- [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md)
  — what the simulator is doing while the benchmark runs.
- [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md)
  — the dispatch overhead the benchmark measures and the cache
  reduces.
- [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
  — the cache and atomic-correctness designs whose effects show up
  in this benchmark.
