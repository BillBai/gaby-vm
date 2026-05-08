# Generate version.cc from the template so PROJECT_VERSION is the single
# source of truth at build time.
configure_file(version.cc.in ${CMAKE_CURRENT_BINARY_DIR}/version.cc @ONLY)

add_library(gaby_vm STATIC)
add_library(gaby_vm::gaby_vm ALIAS gaby_vm)

target_sources(gaby_vm PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.cc)

# Imported VIXL-derived sources. Populated as files land in src/ and
# src/aarch64/ during the import (paths are relative to this CMakeLists.txt).
# These sources compile as part of gaby_vm directly, with relaxed warnings
# applied per-file via gaby_vm_apply_imported_compile_flags() below.
set(GABY_VM_IMPORTED_SOURCES
  # Shared root (Tier 1)
  compiler-intrinsics-vixl.cc
  cpu-features.cc
  utils-vixl.cc

  # AArch64 simulator core (Tier 1)
  aarch64/logic-aarch64.cc
  aarch64/pointer-auth-aarch64.cc
  aarch64/simulator-aarch64.cc

  # Decoder + instruction metadata (Tier 1)
  aarch64/decoder-aarch64.cc
  aarch64/instructions-aarch64.cc

  # CPU-features auditor (Tier 1)
  aarch64/cpu-features-auditor-aarch64.cc

  # Registers / operands (Tier 1)
  aarch64/operands-aarch64.cc
  aarch64/registers-aarch64.cc

  # CPU model (Tier 1)
  aarch64/cpu-aarch64.cc

  # Disassembler (Tier 2 — Simulator constructor depends on it)
  aarch64/disasm-aarch64.cc

  # Debugger (Tier 3 — Simulator constructor depends on it)
  aarch64/debugger-aarch64.cc
)

target_sources(gaby_vm PRIVATE ${GABY_VM_IMPORTED_SOURCES})
gaby_vm_apply_imported_compile_flags(${GABY_VM_IMPORTED_SOURCES})

target_include_directories(gaby_vm
  PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src)

# VIXL build defines, scoped PRIVATE so they do not leak to consumers via
# INTERFACE propagation. VIXL_DEBUG follows CMAKE_BUILD_TYPE. Out-of-scope
# defines (VIXL_INCLUDE_TARGET_A32*, VIXL_CODE_BUFFER_*) are intentionally
# never set; their corresponding code is not imported.
target_compile_definitions(gaby_vm PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)

target_compile_features(gaby_vm PUBLIC cxx_std_17)

gaby_vm_apply_compile_flags(gaby_vm)
