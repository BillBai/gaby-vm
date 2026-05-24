## 1. CMake plumbing

- [x] 1.1 In `bench/CMakeLists.txt`, append a `GABY_VM_BENCH_BUILD_TYPE="$<CONFIG>"` entry to the existing `target_compile_definitions(bench_runner PRIVATE …)` block. Add a one-line comment citing `design.md` Decision 1.

## 2. Refactor `ParseArgs` to a tri-state result

- [x] 2.1 In `bench/runner.cc`, define `enum class ParseResult { kRun, kHelpAndExit, kErrorAndExit };` and a `void PrintUsage(const char* program_name, const char* workload_description, std::FILE* out)` declaration in the anonymous namespace.
- [x] 2.2 Change `ParseArgs` from `bool ParseArgs(int, char*[], Args*)` to `ParseResult ParseArgs(int, char*[], Args*)`. Return `kRun` on success, `kErrorAndExit` on parse failures. (Help handling lands in §3.)
- [x] 2.3 Update `RunBenchmark` to switch on the `ParseResult`: `kHelpAndExit` → `PrintUsage(...); return 0;`, `kErrorAndExit` → `return 2;`, `kRun` → continue. Remove the `if (!ParseArgs(...)) return 2;` shape.

## 3. `--help` / `-h` flag

- [x] 3.1 In `ParseArgs`, scan `argv` once for `--help` or `-h` before the main loop. If present (anywhere in argv), return `kHelpAndExit` immediately.
- [x] 3.2 Implement `PrintUsage` per `design.md` Decision 6 (Usage line, one-line description, flag list with default 5.0 and minimum 0.001 for `--seconds`).
- [x] 3.3 Plumb a workload-description string through `RunBenchmark` so each binary's help text mentions its workload (`mixed`, `smoke`). Smallest viable plumbing: add a `const char* workload_description` parameter to `RunBenchmark`; `bench/baseline.cc` and `bench/smoke.cc` pass a one-sentence description.
- [x] 3.4 Update the unknown-argument `fprintf` in `ParseArgs` from `"unknown argument: %s\n"` to a message that also references `--help`, e.g. `"unknown argument: %s (use --help for the supported flag list)\n"`.

## 4. `--seconds` minimum bound

- [x] 4.1 Add `constexpr double kMinSeconds = 0.001;` near the existing `kDefaultRunSeconds`.
- [x] 4.2 In `ParseArgs`, after parsing the `--seconds` value, reject `s < kMinSeconds` (in addition to the existing `s <= 0.0` check) with `std::fprintf(stderr, "--seconds value %g is below minimum %g\n", s, kMinSeconds);` and return `kErrorAndExit`.

## 5. Emit `build_type` in output

- [x] 5.1 Add at top of `runner.cc`:
  ```cpp
  #ifndef GABY_VM_BENCH_BUILD_TYPE
  #  define GABY_VM_BENCH_BUILD_TYPE "Unknown"
  #endif
  ```
  and a `kBuildType` constant initialized to `*GABY_VM_BENCH_BUILD_TYPE ? GABY_VM_BENCH_BUILD_TYPE : "Unknown"`.
- [x] 5.2 In the output block, print `build_type: <kBuildType>` immediately after the `workload:` line (per spec ordering "alongside `workload`").

## 6. Documentation

- [x] 6.1 In `bench/README.md`, add `--help` / `-h` to the Invocation section (alongside `--seconds <float>`).
- [x] 6.2 In the same section, document the `0.001` minimum that `--seconds` enforces and the resulting exit-2 behavior.
- [x] 6.3 In the output-keys table, add a `build_type` row positioned right after `workload`.

## 7. Verify

- [x] 7.1 `cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON && cmake --build --preset dev-release` builds cleanly with no new warnings (per `cmake/CompileFlags.cmake`).
- [x] 7.2 `./build/release/bench/bench_baseline --help` prints a usage block listing `--seconds`, `--help`, `-h`, and the `5.0` default, then exits 0. *(spec: "--help prints usage and exits 0")*
- [x] 7.3 `./build/release/bench/bench_smoke -h` matches §7.2 (program name differs). *(spec: "-h is a synonym for --help")*
- [x] 7.4 `./build/release/bench/bench_baseline --bogus` writes an error to stderr that names `--bogus` and references `--help`, exits 2. *(spec: "Unknown argument error references --help")*
- [x] 7.5 `./build/release/bench/bench_smoke --seconds 0.0005` writes an error naming the `0.001` minimum to stderr, exits 2 in well under 100 ms (no warm-up runs). *(spec: "Below-minimum --seconds is rejected")*
- [x] 7.6 `./build/release/bench/bench_smoke --seconds 0.2` produces output containing `build_type: Release` and `workload: smoke`, exits 0. *(spec: "All required keys are present in the output", "build_type reflects the CMake configuration")*
- [x] 7.7 Reconfigure with `cmake --preset dev-debug -DGABY_VM_BUILD_BENCHMARKS=ON && cmake --build --preset dev-debug && ./build/debug/bench/bench_smoke --seconds 0.2` produces `build_type: Debug`. *(spec: "build_type reflects the CMake configuration", second clause)*
- [x] 7.8 `openspec validate bench-runner-enhancements --strict` passes.
- [x] 7.9 `ctest` (Release) passes — no regressions in the existing correctness suite.
