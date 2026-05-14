## 1. CMake scaffold and build option

- [x] 1.1 Add `option(GABY_VM_BUILD_BENCHMARKS "Build gaby-vm benchmarks" ${PROJECT_IS_TOP_LEVEL})` to the top-level `CMakeLists.txt`, immediately after the existing `GABY_VM_BUILD_DEMOS` option line, preserving alphabetical/declared order.
- [x] 1.2 Add `if(GABY_VM_BUILD_BENCHMARKS) add_subdirectory(bench) endif()` block at the bottom of the top-level `CMakeLists.txt`, mirroring the existing `GABY_VM_BUILD_TESTS` / `GABY_VM_BUILD_DEMOS` blocks.
- [x] 1.3 Create the `bench/` directory and an initial `bench/CMakeLists.txt` containing only a `message(STATUS "Building gaby-vm benchmarks")` placeholder, so a configure with `GABY_VM_BUILD_BENCHMARKS=ON` succeeds before any source is added.
- [x] 1.4 Configure with `cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON` and confirm the placeholder message fires; configure with `=OFF` and confirm `bench/` is not entered.

## 2. Shared runner

- [x] 2.1 Create `bench/runner.h` declaring a single public entry point (e.g., `int RunBenchmark(const char* workload_name, const char* generator_tag, const uint32_t* code, size_t static_word_count, uint64_t dynamic_insns_per_iter, int argc, char* argv[])`) that takes the per-binary workload metadata and the program args.
- [x] 2.2 Create `bench/runner.cc` implementing: CLI parsing for `--seconds <float>` (default 5.0); simulator construction with `CPUFeatures::All()`; copying the const code into a simulator-visible buffer; setting `LR = Simulator::kEndOfSimAddress` before each `RunFrom`; one discarded warm-up call; steady-state loop bounded by `std::chrono::steady_clock`; output of the spec-mandated key/value lines.
- [x] 2.3 Add an `add_library(bench_runner OBJECT runner.cc)` target to `bench/CMakeLists.txt` with the same `target_include_directories` and `target_compile_definitions` pattern as `test/CMakeLists.txt` (PRIVATE include of `${PROJECT_SOURCE_DIR}/src`; PRIVATE defines `VIXL_INCLUDE_TARGET_A64`, `VIXL_INCLUDE_SIMULATOR_AARCH64`, `$<$<CONFIG:Debug>:VIXL_DEBUG>`).
- [x] 2.4 Link `bench_runner` against whichever simulator library target `test/simulator_correctness` consumes; verify by `cmake --build` that `runner.cc` compiles cleanly under `dev-release`.

## 3. Smoke workload (offline, no upstream VIXL needed)

- [x] 3.1 Author `bench/workloads/smoke_workload.s` — a short AArch64 assembly source containing: `stp x29, x30, [sp, #-16]!` + `mov x29, sp` prologue, ~28 straight-line ALU instructions (e.g., alternating `add x0, x0, #1` / `eor x1, x1, x2` / `orr x3, x3, x4`) with no branches, an `ldp x29, x30, [sp], #16` + `ret` epilogue. Target ~32 total instructions in the body.
- [x] 3.2 Document the `llvm-mc -arch=aarch64 -filetype=obj smoke_workload.s -o smoke.o && llvm-objcopy -O binary --only-section=.text smoke.o smoke.bin` invocation in a comment block at the top of the `.s` file. *(Adjusted to `-triple=aarch64-linux-gnu` to force ELF on a macOS host so `.text` section name matches; documented in the .s header.)*
- [x] 3.3 Run that toolchain locally, convert `smoke.bin` into `constexpr uint32_t[]` literals, and write `bench/workloads/smoke_workload_data.h` with `kSmokeWorkloadInstructions`, `kSmokeWorkloadStaticWordCount`, `kSmokeWorkloadDynamicInstructionsPerIteration` (equal to static count by construction), and `kSmokeWorkloadGeneratorTag` (containing `llvm-mc --version` short string + a digest of `smoke_workload.s`).
- [x] 3.4 Manually verify by inspection that the header's constants match the schema declared in the spec (four constants, correct types).

## 4. Smoke binary (end-to-end harness validation)

- [x] 4.1 Create `bench/smoke.cc` whose `main` includes `smoke_workload_data.h` and calls `RunBenchmark("smoke", kSmokeWorkloadGeneratorTag, kSmokeWorkloadInstructions, kSmokeWorkloadStaticWordCount, kSmokeWorkloadDynamicInstructionsPerIteration, argc, argv)`.
- [x] 4.2 Add `add_executable(bench_smoke smoke.cc) target_link_libraries(bench_smoke PRIVATE bench_runner ...)` to `bench/CMakeLists.txt`; explicitly do NOT call `add_test`.
- [x] 4.3 Build with `cmake --build --preset dev-release`; run `./build/release/bench/bench_smoke --seconds 0.2`; confirm clean exit, non-zero iteration count, and all spec-mandated output keys present.
- [x] 4.4 At this checkpoint the harness pipeline (timing loop, LR convention, output format) is end-to-end green, independent of the mixed workload generation.

## 5. Mixed workload (offline, requires `../vixl` checkout)

- [x] 5.1 In a developer scratch location outside this repo (e.g., `~/scratch/gaby-vm-bench-gen/`), author `gen_mixed_workload.cc` that: constructs a `MacroAssembler` with `CPUFeatures::All()` and a seeded RNG (seed = 0); calls `BenchCodeGenerator(&masm).Generate(256 * KBytes)`; finalizes; constructs a `Simulator` with `CPUFeatures::All()`; enables instruction counting via `set_trace_parameters` configured to count callbacks rather than print; runs `RunFrom(start)` once; captures the dynamic instruction count; emits the C++ header (array literal + four `constexpr` lines + provenance tag) to stdout. *(Counting uses a `DecoderVisitor` (the alternative noted in design.md decision 2), since `SetTraceParameters` enables printing not callback hooks. RNG seed is 42 — hardcoded inside upstream BenchCodeGenerator.)*
- [x] 5.2 Build that program against `../vixl/` using `scons simulator=aarch64 mode=release` per the methodology doc; capture the upstream commit hash of `../vixl/` for the provenance tag. *(Reused pre-existing scons build at `obj/target_a64a32t32/...mode_release.../libvixl.a`; commit `3fe168632164`.)*
- [x] 5.3 Run the generator, redirect stdout to `bench/workloads/mixed_workload_data.h`; confirm the four constants are populated and the provenance string includes the upstream commit hash + seed + buffer size.
- [x] 5.4 Verify by inspection that `kMixedWorkloadDynamicInstructionsPerIteration > 0` and **differs from** `kMixedWorkloadStaticWordCount` (mixed workload has internal control flow, so the two must differ — if equal, the counting hook is wired wrong). *(dynamic=64643, static=65548; differ by ~1.4%.)*

## 6. Baseline binary

- [x] 6.1 Create `bench/baseline.cc` whose `main` includes `mixed_workload_data.h` and calls `RunBenchmark("mixed", kMixedWorkloadGeneratorTag, kMixedWorkloadInstructions, kMixedWorkloadStaticWordCount, kMixedWorkloadDynamicInstructionsPerIteration, argc, argv)`.
- [x] 6.2 Add `add_executable(bench_baseline baseline.cc) target_link_libraries(bench_baseline PRIVATE bench_runner ...)` to `bench/CMakeLists.txt`; explicitly do NOT call `add_test`.
- [x] 6.3 Build and run `./build/release/bench/bench_baseline --seconds 1.0`; confirm clean exit and an order-of-magnitude-reasonable `iterations_per_second` value. *(115.6 iter/s, 7.5M insn/s, 133.8 ns/insn at --seconds 1.0; order-of-magnitude sane.)*

## 7. README

- [x] 7.1 Write `bench/README.md` with the following sections:
  - **Build**: `cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON && cmake --build --preset dev-release`.
  - **Invocation**: how to run `bench_baseline` and `bench_smoke`; what each binary is for; meaning of each output key (workload, generator tag, static/dynamic counts, iterations, elapsed seconds, primary iterations_per_second, derived throughput_insn_per_sec and ns_per_instruction).
  - **Regenerating `smoke_workload_data.h`**: the `llvm-mc` + `llvm-objcopy` pipeline; pointer to `smoke_workload.s`.
  - **Regenerating `mixed_workload_data.h`**: the out-of-tree generator program (full source inlined or referenced), the upstream-VIXL `scons` invocation, and where the upstream commit hash lands in the provenance tag.
  - **Cross-running upstream `bench-mixed-sim`**: how to build it in `../vixl/` (`scons simulator=aarch64 mode=release aarch64_benchmarks`), how to invoke `obj/release/benchmarks/aarch64/bench-mixed-sim`, and which output line to compare against `iterations_per_second`.
  - **Host hygiene**: one short paragraph — "prefer a mostly-idle machine, don't run on battery, expect ±tens-of-percent run-to-run variance." Explicitly note that V1 targets order-of-magnitude correctness, not publication-grade numbers; no core pinning or governor protocol prescribed.

## 8. Verification against spec scenarios

- [x] 8.1 Configure with `-DGABY_VM_BUILD_BENCHMARKS=ON`; build; confirm `bench_baseline` and `bench_smoke` executables exist. *(Spec: "Two benchmark binaries are produced under bench/".)* — both binaries exist at `build/release/bench/`.
- [x] 8.2 Reconfigure with `-DGABY_VM_BUILD_BENCHMARKS=OFF`; confirm no `bench_*` target exists. *(Spec: option turned off removes all benchmark targets.)* — verified in a fresh `build/release-off/` configure; `find build/release-off -name 'bench_*'` returns empty.
- [x] 8.3 Run `ctest -N` in the ON-configured build; confirm neither `bench_baseline` nor `bench_smoke` is listed. *(Spec: "Benchmark binaries are not registered with CTest".)* — `ctest -N` lists only `smoke`, `simulator_smoke`, `simulator_correctness`.
- [x] 8.4 Run `bench_smoke` with no arguments; confirm `elapsed_seconds >= 5.0` and that the default duration matches the spec. *(Spec: "Default duration is 5 seconds".)* — observed `elapsed_seconds: 5.000000`.
- [x] 8.5 Run `bench_smoke --seconds 1.0`; confirm `elapsed_seconds` is approximately 1.0 and meaningfully below the default. *(Spec: "--seconds overrides the default".)* — observed `elapsed_seconds: 1.000001`.
- [x] 8.6 Inspect `bench/runner.cc` and confirm each `RunFrom` call site is preceded by an `LR = kEndOfSimAddress` write. *(Spec: "Runner writes LR before every RunFrom site".)* — runner.cc lines 78→79 (warm-up) and 85→86 (steady-state loop) both pair `WriteLr` immediately before `RunFrom`.
- [x] 8.7 Parse the output of a `bench_baseline` run; numerically verify that `ns_per_instruction == elapsed_seconds * 1e9 / (iterations * dynamic_instructions_per_iteration)` to floating-point tolerance, AND that substituting `static_words_in_buffer` for the dynamic count produces an inconsistent (different) value. *(Spec: "ns_per_instruction uses dynamic count, not static count".)* — reported 145.786355; dynamic-formula 145.786325 (Δ ≈ 3e-5); static-formula 143.773501 (≈ 2 ns off, well outside tolerance).
- [x] 8.8 Parse output of both binaries; confirm every key listed in the spec is present in each. *(Spec: "All required keys are present in the output".)* — all 10 keys present in both `bench_baseline` and `bench_smoke` output.
- [x] 8.9 Run `git diff --name-only main...HEAD` (or against the merge base) and confirm every changed/added path is either the top-level `CMakeLists.txt`, under `bench/`, or under `openspec/changes/baseline-benchmark-harness/`. No `src/`, no `include/gaby_vm/`, no imported VIXL paths. *(Spec: "Change does not modify src/, include/gaby_vm/, or imported VIXL files".)* — this-change paths: root `CMakeLists.txt` (+5 lines: one option + one block); `bench/**` (all new); `openspec/changes/baseline-benchmark-harness/tasks.md` (status updates only). `git diff HEAD -- src/ include/` is empty.
- [x] 8.10 Confirm `bench/README.md` exists and covers each documented section (build, invocation, mixed regeneration, smoke regeneration, upstream cross-comparison). *(Spec: "README covers each documented procedure".)* — all 5 headings present plus a Host hygiene paragraph.
