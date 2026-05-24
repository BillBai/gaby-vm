## Context

The `bench/` harness today is a single CLI accepting one flag
(`--seconds <float>`) and emitting ten `key: value` output lines.
`ParseArgs` returns a `bool` ("ok / not ok"); on `false` the caller exits
2. There is no third state for "valid invocation that should exit cleanly
without running" (i.e., `--help`), and the build configuration that
compiled the binary is invisible to consumers of its output. This change
closes both gaps and tightens `--seconds` validation, all without touching
`src/`, the public headers, or imported VIXL files.

## Goals / Non-Goals

**Goals:**

- Make the harness CLI self-documenting — `--help` works, unknown args
  point to it.
- Make benchmark output reproducible-by-inspection — a reader of the
  output can tell what configuration produced it.
- Tighten the `--seconds` contract so absurdly small values produce a
  clean error rather than meaningless numbers.
- Keep the harness's external contract additive: no removed keys, no
  changed exit codes for currently-supported invocations.

**Non-Goals:**

- A performance regression / gate harness (out of scope; `bench/` stays
  developer-invoked).
- Compiler identity / version, CPU model, or git SHA in the output. The
  single new key (`build_type`) is the minimum useful piece for
  distinguishing perf bug reports; everything else is YAGNI for V1.
- Any generic CLI parsing framework. The harness has two flags total; a
  hand-written parser is the right size.
- Changes to `bench_baseline` or `bench_smoke` themselves — the diff
  lands entirely in the shared `bench_runner` translation unit.

## Decisions

### Decision 1: Use `$<CONFIG>` generator expression to inject the build type, not `${CMAKE_BUILD_TYPE}`

`${CMAKE_BUILD_TYPE}` is empty under multi-config generators (Xcode,
Ninja Multi-Config, Visual Studio) at configure time — only single-config
generators (Make, regular Ninja) populate it. `$<CONFIG>` is a generator
expression that resolves per-config at build time and works for both. We
add a single line to `bench/CMakeLists.txt`:

```cmake
target_compile_definitions(bench_runner PRIVATE
  GABY_VM_BENCH_BUILD_TYPE="$<CONFIG>")
```

`runner.cc` references `GABY_VM_BENCH_BUILD_TYPE` (a string literal at
compile time). The define lives only on `bench_runner` because `runner.cc`
is the only file that uses it; `bench_smoke` and `bench_baseline` see the
constant through the shared OBJECT library without needing per-target
plumbing.

### Decision 2: Sentinel `"Unknown"` for unconfigured builds, never an empty value

Single-config generators with no `-DCMAKE_BUILD_TYPE` set leave
`$<CONFIG>` empty. An empty value (`build_type:`) is indistinguishable
from a parser bug; the `Unknown` sentinel preserves greppability and
signals "default, no config selected" unambiguously. Implementation: a
tiny preprocessor guard in `runner.cc`:

```cpp
#ifndef GABY_VM_BENCH_BUILD_TYPE
#  define GABY_VM_BENCH_BUILD_TYPE "Unknown"
#endif
namespace { constexpr const char* kBuildType =
    *GABY_VM_BENCH_BUILD_TYPE ? GABY_VM_BENCH_BUILD_TYPE : "Unknown"; }
```

The runtime ternary handles the case where CMake injected an empty
string (multi-config without an active config); the `#ifndef` handles the
case where someone builds `runner.cc` outside CMake entirely.

### Decision 3: Refactor `ParseArgs` to return a tri-state `ParseResult`

The current `bool` return collapses three distinct outcomes: "parsed,
run", "parsed, exit cleanly" (new — `--help`), "error, exit with status
2". Rather than papering over with sentinel side-effects, change the
contract:

```cpp
enum class ParseResult { kRun, kHelpAndExit, kErrorAndExit };
ParseResult ParseArgs(int argc, char* argv[], Args* out);
```

`RunBenchmark` switches on the result: `kHelpAndExit` → `PrintUsage()` to
stdout, return 0; `kErrorAndExit` → return 2 (the error message was
already printed by `ParseArgs` to stderr, including a "see --help" hint);
`kRun` → continue. This keeps argument-classification logic in one place
and removes the temptation to thread "should we exit?" booleans through
the runner.

### Decision 4: Minimum `--seconds = 0.001`

Picking the floor is a judgement call. Ruled out:

- `0` (or `<= 0`): already rejected by the existing check; the failure
  mode this addresses is positive-but-meaningless values.
- `0.01` (10 ms): conservatively safe but rejects legitimate
  microbenchmarks of the harness itself (the smoke workload runs in
  single-digit ms — see `bench/README.md` "Completes in milliseconds").
- `0.1`: too aggressive; rejects valid smoke runs.

`0.001` (1 ms) is the smallest value that gives the timed loop at least
a few `RunFrom` iterations even on the smoke workload, while still
rejecting clearly-broken inputs (`1e-9`, `1e-30`). The exact constant is
documented in the spec, the error message, and the README so the
contract is testable. If a future user has a legitimate sub-millisecond
use case, lowering the floor is a one-line change — far cheaper than
not having one.

### Decision 5: `--help` recognized at any argument position, before any parsing

Standard CLI convention: `--help` short-circuits everything else. We
scan `argv` once for `--help`/`-h` before the main parsing loop and
return `kHelpAndExit` immediately. This means
`bench_baseline --seconds bogus --help` prints help (and exits 0) rather
than failing on `bogus` first — matches user expectation and avoids
confusing error-then-help interactions.

### Decision 6: Help text is a terse Usage block, not a man-page reproduction

Format: one `Usage:` line, one short description line, a flag list with
one line per flag and its default. Total ~10 lines. The README is
canonical for invocation; the binary's `--help` is for "what flags exist
and what's their default", not a tutorial. Example:

```
Usage: bench_baseline [--seconds <float>] [--help|-h]

Drive the imported VIXL AArch64 simulator over the mixed workload and
report throughput.

Options:
  --seconds <float>   Timed-loop target duration in seconds.
                      Default: 5.0. Minimum: 0.001.
  --help, -h          Show this message and exit.
```

`bench_smoke`'s help differs only in the program name and the workload
description.

## Risks / Trade-offs

- **Output schema growth.** Any consumer parsing benchmark output now
  sees a new key (`build_type`). The risk is that strict parsers reject
  unknown keys. Mitigation: there is currently no in-tree consumer; the
  schema is documented as "at least the following keys" (see existing
  requirement), which allows append-only growth without breaking the
  contract.
- **Refactor surface in a tiny file.** Switching `ParseArgs` from `bool`
  to `ParseResult` touches its sole caller. The benefit (clean tri-state)
  outweighs the churn since the change is bench-local and the file is
  ~110 lines.
- **The 1 ms floor is partly arbitrary.** Could be too restrictive for
  someone profiling parser overhead. Accepted because the floor is a
  documented constant, easy to lower if a real use case appears, and zero
  today's users hit it.
- **`$<CONFIG>` is opaque to people unfamiliar with CMake generator
  expressions.** Mitigation: a one-line comment in `bench/CMakeLists.txt`
  next to the new define, citing this design doc.
