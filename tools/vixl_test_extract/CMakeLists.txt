# Authorship-time only. Builds the VIXL test-extraction tool, which links the
# reference VIXL (Tier-0 MacroAssembler + Simulator) compiled straight from
# VIXL_SRC_DIR. NEVER part of the shipping build: the whole subdirectory is
# guarded by GABY_VM_BUILD_VIXL_EXTRACT at the top level.
#
# It compiles VIXL's sources directly (rather than linking a prebuilt libvixl.a)
# so the simulator semantics used to capture fixtures are built with the exact
# same toolchain/stdlib as gaby-vm — no g++/libstdc++-vs-clang/libc++ ABI risk.

if(NOT VIXL_SRC_DIR)
  message(FATAL_ERROR
    "vixl_test_extract requires -DVIXL_SRC_DIR=/path/to/vixl (the reference tree)")
endif()
if(NOT EXISTS "${VIXL_SRC_DIR}/src/aarch64/simulator-aarch64.cc")
  message(FATAL_ERROR "VIXL_SRC_DIR='${VIXL_SRC_DIR}' does not look like a VIXL tree")
endif()

# VIXL sources needed for MacroAssembler + Simulator + Decoder (A64). gaby-vm
# imports the simulator subset; the assembler/code-buffer/SVE files below are
# the additions the extraction tool needs (it assembles, gaby-vm does not).
set(_vixl_srcs
  ${VIXL_SRC_DIR}/src/code-buffer-vixl.cc
  ${VIXL_SRC_DIR}/src/compiler-intrinsics-vixl.cc
  ${VIXL_SRC_DIR}/src/cpu-features.cc
  ${VIXL_SRC_DIR}/src/utils-vixl.cc
  ${VIXL_SRC_DIR}/src/aarch64/assembler-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/assembler-sve-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/cpu-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/cpu-features-auditor-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/debugger-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/decoder-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/disasm-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/instructions-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/logic-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/macro-assembler-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/macro-assembler-sve-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/operands-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/pointer-auth-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/registers-aarch64.cc
  ${VIXL_SRC_DIR}/src/aarch64/simulator-aarch64.cc)

# VIXL test infrastructure (Test linked list + RegisterDump + Equal* helpers).
set(_vixl_test_srcs
  ${VIXL_SRC_DIR}/test/test-runner.cc
  ${VIXL_SRC_DIR}/test/test-utils.cc
  ${VIXL_SRC_DIR}/test/aarch64/test-utils-aarch64.cc)

# The upstream test bodies under capture. Phase 0 uses a small sample file;
# Phase 1+ swap in an include-stripped copy of the real upstream .cc.
set(_capture_test_srcs
  phase0_sample_tests.cc)

add_executable(vixl_test_extract
  extract_main.cc
  capture_state.cc
  fixture_writer.cc
  ${_capture_test_srcs}
  ${_vixl_srcs}
  ${_vixl_test_srcs})

target_include_directories(vixl_test_extract PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${VIXL_SRC_DIR}/src
  ${VIXL_SRC_DIR}/test
  ${VIXL_SRC_DIR}/test/aarch64)

# Simulator mode, A64 only. VIXL_DEBUG is deliberately NOT set: it would turn
# VIXL_UNIMPLEMENTED() into an abort, killing the whole run when one test body
# hits an unimplemented form. We want to capture-then-skip instead.
target_compile_definitions(vixl_test_extract PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  # The tool only assembles into a buffer (the body bytes are then copied into
  # the fixture); it never executes host code, so a plain malloc code buffer is
  # enough — no mmap / RWX. gaby-vm itself never needs this (it does not assemble).
  VIXL_CODE_BUFFER_MALLOC)

# Match VIXL's tested C++17 (avoids the C++20 deprecated-enum-enum warnings the
# main build silences per-file; this tool is offline so it stays simple).
set_target_properties(vixl_test_extract PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

# Imported VIXL + test sources carry their own warning profile; silence them so
# the tool's own diagnostics stand out. The tool's own .cc keep default flags.
set_source_files_properties(${_vixl_srcs} ${_vixl_test_srcs} ${_capture_test_srcs}
  PROPERTIES COMPILE_OPTIONS "-w")

# VIXL's test-runner.cc ships its own main() (the upstream test driver). We only
# want its Test linked-list + Test::run() machinery and provide our own main in
# extract_main.cc, so rename the upstream main out of the way for this TU.
set_source_files_properties(${VIXL_SRC_DIR}/test/test-runner.cc
  PROPERTIES COMPILE_DEFINITIONS "main=vixl_test_runner_unused_main")
