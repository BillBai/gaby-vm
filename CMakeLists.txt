cmake_minimum_required(VERSION 3.21)

project(gaby_vm
  DESCRIPTION "Embeddable AArch64 instruction interpreter built on VIXL semantics"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GABY_VM_BUILD_TESTS "Build gaby-vm tests" ${PROJECT_IS_TOP_LEVEL})
option(GABY_VM_BUILD_DEMOS "Build gaby-vm demos" ${PROJECT_IS_TOP_LEVEL})
option(GABY_VM_BUILD_BENCHMARKS "Build gaby-vm benchmarks" ${PROJECT_IS_TOP_LEVEL})
# Dev-only "relative to native" baseline. OFF by default: it executes guest
# AArch64 code directly on the host (host must be arm64), needs executable
# memory (MAP_JIT + a JIT entitlement on Apple), and is consumed inside the
# bench subdirectory, so it also requires GABY_VM_BUILD_BENCHMARKS.
option(GABY_VM_BUILD_NATIVE_BASELINE
       "Build the dev-only native (host-CPU) workload baseline tools" OFF)
# iOS runner: build the vixl_port family bodies as libraries (no main, no CTest
# executables) so the iOS XCTest host can link them and call the suite entries.
# Pulls in the TEST-ONLY VIXL assembler island without enabling the CTest path.
option(GABY_VM_BUILD_IOS_RUNNER
       "Build the vixl_port family libraries for the iOS runner (no CTest exes)" OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileFlags.cmake)

add_subdirectory(Sources/gaby_vm/src)

if(GABY_VM_BUILD_TESTS)
  enable_testing()
endif()

# The TEST-ONLY VIXL assembler island (and, under GABY_VM_BUILD_IOS_RUNNER, the
# vixl_port family libraries) serve both the CTest path and the iOS runner.
# Added before test/ so the island target and its link helper exist first.
if(GABY_VM_BUILD_TESTS OR GABY_VM_BUILD_IOS_RUNNER)
  add_subdirectory(test/test_support/vixl_asm)
endif()

# test/ holds the CTest suites and, under GABY_VM_BUILD_IOS_RUNNER, the baseline
# suite libraries for the iOS runner. The CTest executables stay gated on
# GABY_VM_BUILD_TESTS inside the subdirectory.
if(GABY_VM_BUILD_TESTS OR GABY_VM_BUILD_IOS_RUNNER)
  add_subdirectory(test)
endif()

if(GABY_VM_BUILD_DEMOS)
  add_subdirectory(demos/cli)
endif()

# bench/ holds the developer benchmark executables and, under
# GABY_VM_BUILD_IOS_RUNNER, the business-bench library for the iOS runner
# (cache/decoder only — no native baseline). The shared bench_runner builds for
# both; the executables stay gated on GABY_VM_BUILD_BENCHMARKS inside.
if(GABY_VM_BUILD_BENCHMARKS OR GABY_VM_BUILD_IOS_RUNNER)
  add_subdirectory(bench)
endif()
