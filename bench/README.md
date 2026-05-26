# gaby-vm benchmarks

This directory is a developer-invoked harness that measures the
throughput of a fixed instruction workload through one of two execution
engines: the imported `vixl::aarch64::Simulator` (the **decoder** engine,
default) or the gaby-vm cache-track `Simulator` over a `PredecodeCache`
(the **cache** engine, selected with `--engine cache`). Numbers from
this harness are the project's reference baseline: future changes that
touch either execution path (leaf tweaks, cache changes, etc.) compare
against numbers from this harness, and the same flag picks the engine
to compare under.

There are two binaries:

- **`bench_baseline`** — drives the upstream-VIXL `BenchCodeGenerator` mixed
  workload (~64k dynamically executed instructions per `RunFrom`). This is
  the binary to cite for performance numbers.
- **`bench_smoke`** — drives a 32-instruction straight-line workload
  assembled by `llvm-mc`. Completes in milliseconds. Use it to verify the
  harness pipeline (timing loop, output format) end-to-end without paying
  the full mixed-workload run cost.

Neither binary is registered with CTest — `ctest` is for correctness, not
performance. Run them directly from the build output directory.

## Build

```sh
cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON
cmake --build --preset dev-release
```

This produces `build/release/bench/bench_baseline` and
`build/release/bench/bench_smoke`. The `GABY_VM_BUILD_BENCHMARKS` option
defaults to `${PROJECT_IS_TOP_LEVEL}`, so a top-level configure already
turns it on; embedded consumers add `-DGABY_VM_BUILD_BENCHMARKS=ON`
explicitly when they want the harness.

## Invocation

```sh
./build/release/bench/bench_baseline                          # 5 seconds, decoder engine
./build/release/bench/bench_baseline --seconds 10
./build/release/bench/bench_baseline --engine cache           # cache-on, default --seconds
./build/release/bench/bench_smoke    --engine cache --seconds 0.2
./build/release/bench/bench_baseline --help                   # usage block, exit 0
./build/release/bench/bench_baseline -h                       # short alias for --help
```

Supported flags:

- `--engine {decoder|cache}` — execution engine that drives the
  workload. Default `decoder`, which preserves the historic behaviour:
  the harness drives the imported `vixl::aarch64::Simulator` directly.
  `cache` instead constructs a `gaby_vm::PredecodeCache`, registers the
  workload's instruction buffer as a code range **before** the warm-up
  call, and drives `gaby_vm::Simulator::RunFrom` over it. The one-time
  predecode pass is paid outside the timed region, so the measured loop
  reports steady-state cache *execution* speed — not cache
  *construction* speed.
- `--seconds <float>` — timed-loop target duration. Default `5.0`,
  mirroring upstream `BenchCLI::kDefaultRunTime`. Values strictly less
  than `0.001` (1 ms) are rejected with an error on stderr and exit
  status `2`; the minimum exists to guarantee the timed loop sees at
  least a few `RunFrom` iterations even on the smoke workload.
- `--help`, `-h` — print a usage block to stdout listing the flags and
  defaults, then exit with status `0`. Recognized at any argument
  position; short-circuits all other parsing so that
  `bench_smoke --seconds bogus --help` prints help instead of failing on
  `bogus` first.

Unknown arguments produce an error on stderr that names the offending
argument and points the user at `--help`, then exit status `2`. An
unrecognised `--engine` value (anything other than `decoder` or `cache`)
is rejected the same way.

Each invocation prints one observation as `key: value` lines on stdout:

| Key | Meaning |
|-----|---------|
| `workload` | Short identifier (`mixed` or `smoke`). |
| `build_type` | The CMake configuration that compiled the runner (`Release`, `Debug`, `RelWithDebInfo`, `MinSizeRel`, ...). The string `Unknown` is emitted when no configuration is selected (rare; can happen with single-config generators invoked without `-DCMAKE_BUILD_TYPE`). |
| `engine` | The execution engine that produced the run (`decoder` or `cache`), reflecting the `--engine` flag. Lets a downstream consumer tell cache-on numbers apart from cache-off numbers in a mixed log. |
| `workload_generator_tag` | Provenance string from the workload header — upstream commit + seed + buffer size for `mixed`; `llvm-mc` version + source digest for `smoke`. |
| `static_words_in_buffer` | Number of 4-byte instruction words physically present in the workload header's array. |
| `dynamic_instructions_per_iteration` | Number of guest instructions actually executed by one `Simulator::RunFrom` over the buffer. For `mixed`, captured offline once during workload generation; for `smoke` (branch-free), equals the static count by construction. |
| `iterations` | Number of `RunFrom` calls in the timed region (warm-up call excluded). |
| `total_dynamic_instructions` | `iterations × dynamic_instructions_per_iteration`. |
| `elapsed_seconds` | Wall-clock duration of the timed region, `std::chrono::steady_clock`. |
| `iterations_per_second` | **Primary** metric — `iterations / elapsed_seconds`. Matches upstream `bench-mixed-sim`'s headline number. |
| `throughput_insn_per_sec` | Derived: `total_dynamic_instructions / elapsed_seconds`. |
| `ns_per_instruction` | Derived: `elapsed_seconds × 1e9 / total_dynamic_instructions`. Uses the **dynamic** count; substituting `static_words_in_buffer` for `mixed` gives a wrong answer because the workload contains internal control flow. |

The harness runs one discarded warm-up `RunFrom` before the timed region
and resets `LR` to the end-of-simulation sentinel (`NULL` — i.e. `0` —
for both engines, matching `vixl::aarch64::Simulator::kEndOfSimAddress`)
before every `RunFrom` call (warm-up included), so each iteration starts
from the same exit convention regardless of what the workload did with
`LR`. The warm-up, timed-loop, and LR-reset cadence are identical across
engines; per-engine setup (constructing the simulator, registering the
predecode cache) happens once before the warm-up and so falls outside
the timed region.

## Comparing cache-on vs cache-off throughput

The `--engine` flag exists so the predecode cache can be measured against
the decoder baseline on identical workloads. Both runs go through the
same harness — same workload bytes, same warm-up call, same timed loop
shape, same reported keys — so their `iterations_per_second` numbers are
directly comparable.

A typical comparison run, on either binary:

```sh
./build/release/bench/bench_baseline --engine decoder --seconds 1.0
./build/release/bench/bench_baseline --engine cache   --seconds 1.0
```

The `engine` key in each output identifies which run was which. A
reasonable speedup expectation today is several × on `mixed` and roughly
order-of-magnitude on `smoke` — the smoke workload is branch-free, so
the cache eliminates more relative dispatch overhead per instruction.
Treat those as ballpark sanity checks, not committed targets: the
acceptance criterion is "meaningful, measured improvement", not a fixed
N×. Run each side a few times and look at the order of magnitude,
because run-to-run variance on a typical unspecialized host is tens of
percent (see *Host hygiene* below).

`--seconds 0.2` is enough on `bench_smoke` to see the gap; `bench_baseline`
needs at least `--seconds 1.0` for the cache-on side to see enough
iterations to be stable.

## Regenerating `bench/workloads/smoke_workload_data.h`

The smoke workload is `bench/workloads/smoke_workload.s` (32 instructions:
2-instruction prologue, 28-instruction ALU body, 2-instruction epilogue).
Regenerate the header by assembling the `.s` with `llvm-mc`, extracting
the `.text` section with `llvm-objcopy`, and converting the resulting
binary to `constexpr uint32_t` literals.

```sh
LLVM=/opt/homebrew/opt/llvm/bin   # Homebrew LLVM (keg-only on macOS)
$LLVM/llvm-mc -triple=aarch64-linux-gnu -filetype=obj \
    bench/workloads/smoke_workload.s -o /tmp/smoke.o
$LLVM/llvm-objcopy -O binary --only-section=.text \
    /tmp/smoke.o /tmp/smoke.bin
od -An -tx4 -v /tmp/smoke.bin
```

`-triple=aarch64-linux-gnu` forces ELF output regardless of host so the
`.text` section is named the standard way (Mach-O would name it `__text`,
and `--only-section=.text` would silently keep everything). The resulting
`/tmp/smoke.bin` is exactly 128 bytes (32 little-endian 4-byte words).

Convert those 32 words into the `kSmokeWorkloadInstructions[]` literal
array in `bench/workloads/smoke_workload_data.h`. Update
`kSmokeWorkloadGeneratorTag` with the `llvm-mc` version short string and
a 12-char prefix of `shasum -a 256 bench/workloads/smoke_workload.s`.
The dynamic and static counts both equal 32 by construction (branch-free
body).

## Regenerating `bench/workloads/mixed_workload_data.h`

The mixed workload is regenerated by a one-shot C++ program kept
**outside** this repo — by design the gaby-vm build has no dependency on
upstream VIXL's assembler. The reference generator lives at
`~/scratch/gaby-vm-bench-gen/gen_mixed_workload.cc`. It mirrors the shape
of upstream `benchmarks/aarch64/bench-mixed-sim.cc`: builds a
`MacroAssembler` with `CPUFeatures::All()`, drives upstream's
`BenchCodeGenerator::Generate(256 * KBytes)`, finalizes the code buffer,
constructs a `Simulator`, and runs `RunFrom(start)` once with a counting
`DecoderVisitor` registered on the shared `Decoder` to capture the
dynamic instruction count.

The `DecoderVisitor` choice is the practical resolution of design
decision 2: `SetTraceParameters` would enable per-instruction *printing*
but not a callback hook, so a tiny visitor that simply increments a
counter is the cleanest path to a dynamic count without changing the
simulator.

### Steps

```sh
# 1. Build upstream VIXL (one-time per upstream version).
(cd ../vixl && scons simulator=aarch64 mode=release)

# 2. Build the generator out-of-tree against the libvixl.a above.
cd ~/scratch/gaby-vm-bench-gen
./build.sh   # see gaby-vm/bench/README.md for the compile/link command

# 3. Run, redirect stdout into the committed header.
./gen_mixed_workload > path/to/gaby-vm/bench/workloads/mixed_workload_data.h
```

The reference `build.sh` invocation, suitable to copy into a developer's
own scratch directory:

```sh
#!/bin/sh
set -eu
VIXL_DIR="${VIXL_DIR:-$HOME/Workspace/gaby-vm_workspace/vixl}"
LIBVIXL="$VIXL_DIR/obj/target_a64a32t32/mode_release/symbols_off/compiler_g++/std_c++17/simulator_aarch64/negative_testing_off/code_buffer_allocator_malloc/libvixl.a"
clang++ -std=c++17 -O2 \
    -I"$VIXL_DIR/src" \
    -I"$VIXL_DIR/benchmarks/aarch64" \
    -DVIXL_INCLUDE_TARGET_A64 \
    -DVIXL_INCLUDE_SIMULATOR_AARCH64 \
    -Wno-unused-parameter \
    gen_mixed_workload.cc \
    "$VIXL_DIR/benchmarks/aarch64/bench-utils.cc" \
    "$LIBVIXL" \
    -o gen_mixed_workload
```

The provenance tag emitted into the header records the upstream commit
hash, the RNG seed (hard-coded to 42 inside `BenchCodeGenerator`), and
the actual emitted buffer size (which is typically slightly larger than
`256 * KBytes` because pool entries round up).

The recorded `kMixedWorkloadDynamicInstructionsPerIteration` is the
count under the upstream simulator. If the imported simulator's behavior
diverges from upstream at some future point (a marker edit that changes
a leaf, an upstream bugfix not yet pulled), the recorded count can drift
from what gaby-vm's simulator dispatches at runtime. The primary
`iterations_per_second` metric is directly observed and unaffected; the
derived `ns_per_instruction` would be off proportionally to the drift.
A regeneration after any such divergence re-aligns the count.

## Cross-running upstream `bench-mixed-sim` for comparison

To sanity-check a gaby-vm baseline number against upstream:

```sh
(cd ../vixl && scons simulator=aarch64 mode=release aarch64_benchmarks)
../vixl/obj/target_a64a32t32/mode_release/symbols_off/compiler_g++/std_c++17/simulator_aarch64/negative_testing_off/code_buffer_allocator_malloc/benchmarks/aarch64/bench-mixed-sim
```

Compare upstream's output line (its iterations-per-second figure) against
`bench_baseline`'s `iterations_per_second`. Use the same `--seconds`
value on both for an apples-to-apples comparison; upstream defaults to
5 seconds, same as this harness.

The comparison answers: "is our imported simulator running the same
workload at roughly the same speed as upstream's simulator builds it?"
A large divergence (say >2×) is a signal worth investigating; small
differences (tens of percent) are noise floor on a typical laptop.

## Host hygiene

Prefer a mostly-idle machine, don't run on battery. V1 of this harness
targets order-of-magnitude correctness — "does the cache help by 3×?" —
not publication-grade numbers. No core pinning, turbo control, or
governor tweaks are prescribed; if a future change needs that level of
precision, it's the right place to add a noise-control protocol.

Expect run-to-run variance of tens of percent on a typical
unspecialized host. Run the binary several times and look at the order
of magnitude rather than the trailing digits.
