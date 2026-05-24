cmake_minimum_required(VERSION 3.21)

project(gaby_vm
  VERSION 0.0.1
  DESCRIPTION "Embeddable AArch64 instruction interpreter built on VIXL semantics"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GABY_VM_BUILD_TESTS "Build gaby-vm tests" ${PROJECT_IS_TOP_LEVEL})
option(GABY_VM_BUILD_DEMOS "Build gaby-vm demos" ${PROJECT_IS_TOP_LEVEL})
option(GABY_VM_BUILD_BENCHMARKS "Build gaby-vm benchmarks" ${PROJECT_IS_TOP_LEVEL})

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileFlags.cmake)

add_subdirectory(src)

if(GABY_VM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(GABY_VM_BUILD_DEMOS)
  add_subdirectory(demos/cli)
endif()

if(GABY_VM_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
