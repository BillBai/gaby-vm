message(STATUS "Building gaby-vm benchmarks")

# Shared runner: timing loop, exit-convention setup, and reporting code.
# Both bench_baseline and bench_smoke consume this OBJECT library so the
# per-binary main() reduces to a single RunBenchmark() call (see design.md
# decision 5). The include + compile-definition pattern mirrors
# test/CMakeLists.txt's privileged consumption of the imported VIXL surface:
# PRIVATE include of ${PROJECT_SOURCE_DIR}/src lets runner.cc reach
# vixl::aarch64::Simulator without exposing those headers in include/gaby_vm/,
# and the three VIXL defines must match the ones the simulator code was
# compiled with so the header view is consistent.
add_library(bench_runner OBJECT runner.cc)
target_include_directories(bench_runner PRIVATE ${PROJECT_SOURCE_DIR}/src)
# GABY_VM_BENCH_BUILD_TYPE: $<CONFIG> is a generator expression that resolves
# at build time to the active configuration name (Release, Debug, ...). It is
# the right idiom across both single- and multi-config generators; see
# openspec/changes/bench-runner-enhancements/design.md decision 1.
target_compile_definitions(bench_runner PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>
  GABY_VM_BENCH_BUILD_TYPE="$<CONFIG>")
target_link_libraries(bench_runner PUBLIC gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(bench_runner)

# bench_smoke — millisecond-scale end-to-end harness self-test. Drives the
# llvm-mc-assembled straight-line workload; not registered with CTest
# (benchmarks stay separate from the correctness suite — see spec). The
# PRIVATE include + define block mirrors bench_runner above (per design.md
# decision 7, applied to all three targets) so a future change that adds a
# direct VIXL include to smoke.cc compiles without touching CMake.
add_executable(bench_smoke smoke.cc)
target_link_libraries(bench_smoke PRIVATE bench_runner)
target_include_directories(bench_smoke PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(bench_smoke PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)
gaby_vm_apply_compile_flags(bench_smoke)

# bench_baseline — reference throughput measurement, driven by the
# upstream-VIXL-generated mixed workload. Same harness wiring as bench_smoke;
# the only difference is the included workload header. Not registered with
# CTest.
add_executable(bench_baseline baseline.cc)
target_link_libraries(bench_baseline PRIVATE bench_runner)
target_include_directories(bench_baseline PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(bench_baseline PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)
gaby_vm_apply_compile_flags(bench_baseline)
