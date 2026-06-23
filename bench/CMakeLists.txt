message(STATUS "Building gaby-vm benchmarks")

# Shared runner: timing loop, exit-convention setup, and reporting code.
# Both bench_baseline and bench_smoke consume this OBJECT library so the
# per-binary main() reduces to a single RunBenchmark() call (see design.md
# decision 5). The include + compile-definition pattern mirrors
# test/CMakeLists.txt's privileged consumption of the imported VIXL surface:
# PRIVATE include of ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src lets runner.cc reach
# vixl::aarch64::Simulator (which drives decoder mode) without exposing those
# headers in include/gaby_vm/, and the three VIXL defines must match the ones
# the simulator code was compiled with so the header view is consistent.
#
# Cache mode consumes the gaby_vm public cache API
# (gaby_vm/predecode_cache.h, gaby_vm/simulator.h). Those headers ride in
# transitively through the existing PUBLIC link to gaby_vm::gaby_vm below; no
# extra include directive is required — the link target already publishes
# include/gaby_vm/ to its consumers.
add_library(bench_runner OBJECT runner.cc)
target_include_directories(bench_runner PRIVATE ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src)
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

# The benchmark executables (and the dev-only native baseline) build only for
# the developer benchmark path. The iOS runner reuses bench_runner and the
# business kernels as a library (the GABY_VM_BUILD_IOS_RUNNER block at the end).
if(GABY_VM_BUILD_BENCHMARKS)

# bench_smoke — millisecond-scale end-to-end harness self-test. Drives the
# llvm-mc-assembled straight-line workload; not registered with CTest
# (benchmarks stay separate from the correctness suite — see spec). The
# PRIVATE include + define block mirrors bench_runner above (per design.md
# decision 7, applied to all three targets) so a future change that adds a
# direct VIXL include to smoke.cc compiles without touching CMake.
add_executable(bench_smoke smoke.cc)
target_link_libraries(bench_smoke PRIVATE bench_runner)
target_include_directories(bench_smoke PRIVATE ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src)
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
target_include_directories(bench_baseline PRIVATE ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src)
target_compile_definitions(bench_baseline PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)
gaby_vm_apply_compile_flags(bench_baseline)

# bench_business — business-logic microkernel suite (parse/hash/struct/fsm).
# These compiled-C scalar kernels model the iOS hot-fix scenario (business
# logic, no NEON/syscall/external calls), so their cache-vs-native numbers are
# the representative slowdown figure that smoke (too simple) and mixed (68%
# NEON) cannot give. Beyond the shared runner it reaches the gaby_vm public
# Simulator API directly for its --verify track-equivalence + dynamic-count
# pass, which rides in transitively through bench_runner's PUBLIC gaby_vm link.
# Not registered with CTest (benchmarks stay out of the correctness suite).
add_executable(bench_business business.cc)
target_link_libraries(bench_business PRIVATE bench_runner)
target_include_directories(bench_business PRIVATE ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src)
target_compile_definitions(bench_business PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)
gaby_vm_apply_compile_flags(bench_business)

# size_probe — minimal public-API link unit for the CI binary-size report
# (ci/size-report.sh). NOT a benchmark: it exists only so the size report can
# strip it and measure the footprint an embedder actually pays. Unlike the
# bench_* targets it links ONLY the public gaby_vm::gaby_vm surface — no PRIVATE
# src include, no VIXL defines — and dead-strips unused code so the measured
# size reflects just what the public API pulls in. Not registered with CTest.
add_executable(size_probe size_probe.cc)
target_link_libraries(size_probe PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(size_probe)
if(APPLE)
  target_link_options(size_probe PRIVATE -Wl,-dead_strip)
elseif(NOT MSVC)
  target_link_options(size_probe PRIVATE -Wl,--gc-sections)
endif()

# native_smoke / native_baseline — dev-only "relative to native" reference.
# They copy the SAME committed workload bytes (smoke / mixed) into executable
# memory and run them directly on the host CPU, so decoder- and cache-mode
# numbers can be quoted against native. They do NOT link gaby_vm — there is no
# simulator involved on this path; the only dependency is the shared
# native_runner (MAP_JIT setup + timing loop + reporting).
#
# Opt-in via GABY_VM_BUILD_NATIVE_BASELINE. The host must be arm64 (guest ISA ==
# host ISA); on a non-arm64 host we warn and skip rather than emit a target that
# cannot run. Not registered with CTest — benchmarks stay out of the
# correctness suite.
if(GABY_VM_BUILD_NATIVE_BASELINE)
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    message(WARNING
      "GABY_VM_BUILD_NATIVE_BASELINE is ON but the host processor is "
      "'${CMAKE_SYSTEM_PROCESSOR}', not arm64. The native baseline runs guest "
      "AArch64 code on the host CPU, so it is skipped on non-arm64 hosts.")
  else()
    add_library(native_runner OBJECT native_runner.cc)
    target_compile_definitions(native_runner PRIVATE
      GABY_VM_BENCH_BUILD_TYPE="$<CONFIG>")
    gaby_vm_apply_compile_flags(native_runner)

    add_executable(native_smoke native_smoke.cc)
    target_link_libraries(native_smoke PRIVATE native_runner)
    gaby_vm_apply_compile_flags(native_smoke)

    add_executable(native_baseline native_baseline.cc)
    target_link_libraries(native_baseline PRIVATE native_runner)
    gaby_vm_apply_compile_flags(native_baseline)

    # native_business — host-CPU baseline for the business-logic microkernels.
    # The honest denominator for the slowdown figure: same committed kernel
    # bytes, run directly on the host. Needs the gaby_vm headers only for the
    # committed workload arrays (header-only), so like the other native tools it
    # does NOT link gaby_vm — but it does need the include path to reach
    # bench/workloads/business/*.h via the bench source dir (already the include
    # root for native_*.cc).
    add_executable(native_business native_business.cc)
    target_link_libraries(native_business PRIVATE native_runner)
    gaby_vm_apply_compile_flags(native_business)

    if(APPLE)
      # Apple Silicon executes MAP_JIT pages only for a process carrying the
      # allow-jit entitlement. Adhoc-sign each tool post-build with it; see
      # native_jit.entitlements.
      foreach(_tgt native_smoke native_baseline native_business)
        add_custom_command(TARGET ${_tgt} POST_BUILD
          COMMAND codesign -s - --entitlements
                  "${CMAKE_CURRENT_SOURCE_DIR}/native_jit.entitlements" -f
                  "$<TARGET_FILE:${_tgt}>"
          COMMENT "codesign ${_tgt} with the JIT entitlement (MAP_JIT)"
          VERBATIM)
      endforeach()
    endif()
  endif()
endif()

endif()  # GABY_VM_BUILD_BENCHMARKS

# iOS runner: the business-logic microkernels as a report-only library
# (cache/decoder only — no native baseline). Same source and privileged build as
# the bench_business executable, with main() renamed so the iOS XCTest can call
# gaby_vm_ios_run_business_bench() instead.
if(GABY_VM_BUILD_IOS_RUNNER)
  add_library(gaby_vm_ios_business_bench_lib STATIC business.cc)
  target_link_libraries(gaby_vm_ios_business_bench_lib PRIVATE bench_runner)
  target_include_directories(gaby_vm_ios_business_bench_lib PRIVATE
    ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src)
  target_compile_definitions(gaby_vm_ios_business_bench_lib PRIVATE
    VIXL_INCLUDE_TARGET_A64
    VIXL_INCLUDE_SIMULATOR_AARCH64
    $<$<CONFIG:Debug>:VIXL_DEBUG>
    main=gaby_vm_ios_business_bench_unused_main)
  gaby_vm_apply_compile_flags(gaby_vm_ios_business_bench_lib)
endif()
