# TEST-ONLY VIXL assembler island. NOT part of the gaby_vm shipping library.
#
# Compiles a copy of the Tier-0 VIXL AArch64 assembler (+ the VIXL test
# infrastructure it needs), taken verbatim from ../vixl at the import SHA pinned
# in docs/refs/vixl-extraction-map.md, so the vixl_port suite can assemble each
# upstream TEST() body live at test time. gaby_vm imported only the simulator /
# decoder / leaf chain; this island supplies the missing assembler delta.
#
# How it cannot leak into the shipping library (three independent guarantees):
#   1. Target name has a `_testonly` suffix and is given NO `::` alias, so one
#      grep shows it cannot ship; it is only added under GABY_VM_BUILD_TESTS.
#   2. Sources/gaby_vm/src/CMakeLists.txt declares zero target_link_libraries,
#      so nothing can pull this back into gaby_vm.a.
#   3. Consumers (the family test exes) link it PRIVATE.
#
# ODR safety: the include path puts Sources/gaby_vm/src FIRST, so the copied
# assembler's #include "operands-aarch64.h" / "simulator-aarch64.h" resolve to
# the headers ALREADY compiled into gaby_vm.a (one definition each). We link
# gaby_vm::gaby_vm PRIVATE, so those shared leaf symbols come from gaby_vm.a.
# The .cc files copied here are written against upstream headers but compile
# against gaby's hand-edited ones; the pinned SHA keeps both halves consistent
# (see the extraction map's anti-ODR note).

# Upstream sources compiled into the island archive. These are the assembler
# delta gaby_vm does not have, plus the VIXL test infra (Test linked list +
# RegisterDump). The leaf chain (operands/registers/simulator/decoder/...) is
# NOT re-compiled here — it comes from gaby_vm.a.
set(_island_upstream_srcs
  src/code-buffer-vixl.cc
  src/aarch64/assembler-aarch64.cc
  src/aarch64/assembler-sve-aarch64.cc        # only to satisfy out-of-line symbols
  src/aarch64/macro-assembler-aarch64.cc
  src/aarch64/macro-assembler-sve-aarch64.cc  # only to satisfy out-of-line symbols
  # The four assembler-only Instruction setters gaby's read-only
  # instructions-aarch64.cc gates out (#if 0). gaby_vm.a omits them, so supplying
  # them here is no duplicate — it closes the only real symbol gap the assembler
  # has against gaby_vm.a (see the file's own note).
  src/aarch64/instruction-assembler-setters-aarch64.cc
  test/test-runner.cc
  test/aarch64/test-utils-aarch64.cc)

# gaby-authored island sources (strict warning policy, project style): the
# ExecuteMemory abort-on-call stub and the two-track oracle (gaby public API only).
set(_island_gaby_srcs
  harness/test-utils-stub.cc
  harness/vixl_port_oracle.cc)

add_library(gaby_vm_vixl_asm_testonly STATIC
  ${_island_upstream_srcs}
  ${_island_gaby_srcs})

# Deliberately NO add_library(gaby_vm::vixl_asm ALIAS ...): the missing `::`
# alias is part of the "cannot ship" contract.

target_include_directories(gaby_vm_vixl_asm_testonly PUBLIC
  # gaby_vm's imported leaf + simulator headers resolve FIRST. The second entry
  # (.../src/aarch64) lets the copied assembler's "../globals-vixl.h" join to
  # the gaby root header and resolves the bare gaby siblings (operands-aarch64.h,
  # simulator-aarch64.h, simulator-constants-aarch64.h).
  ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src
  ${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src/aarch64
  # The island's own copied assembler (src/, src/aarch64/) and VIXL test infra
  # (test/, test/aarch64/), plus the gaby-authored harness headers. PUBLIC so the
  # family test exes that #include the upstream bodies inherit the same path.
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/aarch64
  ${CMAKE_CURRENT_SOURCE_DIR}/test
  ${CMAKE_CURRENT_SOURCE_DIR}/test/aarch64
  ${CMAKE_CURRENT_SOURCE_DIR}/harness)

# PRIVATE: gaby_vm's PRIVATE usage requirements do not propagate to island
# consumers, but the static archive still pulls gaby_vm.a in at final link so the
# shared leaf symbols resolve.
target_link_libraries(gaby_vm_vixl_asm_testonly PRIVATE gaby_vm::gaby_vm)

# Same VIXL build mode as gaby_vm (A64 + simulator; VIXL_DEBUG under Debug) so
# the copied assembler sees the imported headers exactly as gaby_vm compiled
# them. VIXL_CODE_BUFFER_MALLOC forces the VIXL code buffer to plain malloc/free
# with no executable-memory path (no mmap/mprotect, no native execution).
# All PRIVATE — none of these may reach a gaby_vm consumer (the island never
# links into gaby_vm, and PRIVATE makes that explicit). VIXL_CODE_BUFFER_MMAP is
# never defined anywhere.
target_compile_definitions(gaby_vm_vixl_asm_testonly PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  VIXL_CODE_BUFFER_MALLOC
  $<$<CONFIG:Debug>:VIXL_DEBUG>)

# VIXL's test-runner.cc ships its own main() (the upstream test driver). We only
# want its Test linked-list + Test::run() machinery; each family harness provides
# its own main(). Rename the upstream main out of the way for this TU.
set_source_files_properties(test/test-runner.cc
  PROPERTIES COMPILE_DEFINITIONS "main=gaby_vm_vixl_asm_island_unused_main")

# Strict project warnings for the whole target (covers the gaby-authored stub)...
gaby_vm_apply_compile_flags(gaby_vm_vixl_asm_testonly)
# ...then silence warnings on the verbatim upstream sources (the trailing -w wins
# over the strict flags above). These files carry their own upstream warning
# profile; relitigating it adds no signal. Matches the proven extraction-tool
# treatment of the same files.
set_source_files_properties(${_island_upstream_srcs}
  PROPERTIES COMPILE_OPTIONS "-w")

# Apply to any test exe that #includes the island's assembler / VIXL headers.
# It:
#   * links the island PRIVATE (so the island cannot leak past the exe), and
#   * links gaby_vm::gaby_vm PRIVATE so the exe inherits gaby_vm's PUBLIC include
#     (Sources/gaby_vm/include) — gaby's imported simulator-aarch64.h includes
#     "gaby_vm/predecode_cache.h", which lives there; the island links gaby_vm
#     PRIVATE so it does NOT re-export that include path, and
#   * re-declares the VIXL build defines the island compiled with. Re-declaring
#     matters for ODR: the island's PRIVATE defines are not propagated, and the
#     shared imported headers carry VIXL_DEBUG-gated inline bodies — a consumer
#     compiling them under a different VIXL_DEBUG would be an ODR violation.
# Matches the per-test "privileged build" pattern already used in test/CMakeLists.txt.
function(gaby_vm_vixl_asm_link_island target)
  target_link_libraries(${target} PRIVATE
    gaby_vm_vixl_asm_testonly
    gaby_vm::gaby_vm)
  target_compile_definitions(${target} PRIVATE
    VIXL_INCLUDE_TARGET_A64
    VIXL_INCLUDE_SIMULATOR_AARCH64
    $<$<CONFIG:Debug>:VIXL_DEBUG>)
  gaby_vm_apply_compile_flags(${target})  # -Wall -Wextra -Wpedantic
  # The exe #includes imported VIXL headers, which legitimately trip these
  # C++20/23-deprecation warnings (same classes gaby_vm_apply_imported_compile_flags
  # silences for the imported .cc). Added AFTER the strict flags so they win.
  target_compile_options(${target} PRIVATE
    -Wno-deprecated-enum-enum-conversion # imported NEON*/Logical* enum bitwise ops
    -Wno-deprecated-literal-operator)    # decoder-constants-aarch64.h operator"" _b
endfunction()

# ODR/link smoke gate (migration step 3), kept as a permanent linkage canary.
add_executable(gaby_vm_vixl_asm_island_smoke harness/island_smoke.cc)
gaby_vm_vixl_asm_link_island(gaby_vm_vixl_asm_island_smoke)
add_test(NAME vixl_asm_island_smoke COMMAND gaby_vm_vixl_asm_island_smoke)

# Two-track harness smoke (migration step 4.4): a trivial inline TEST() body
# driven through gaby_two_track_macros.h + the oracle, proving they compile and
# link against the island before the real upstream families are wired in (§5).
add_executable(gaby_vm_vixl_asm_harness_smoke harness/two_track_harness_smoke.cc)
gaby_vm_vixl_asm_link_island(gaby_vm_vixl_asm_harness_smoke)
add_test(NAME vixl_asm_harness_smoke COMMAND gaby_vm_vixl_asm_harness_smoke)

# Harness self-test: injection-style regression protection for the two-track
# harness's OWN safety logic (the gaby-track crash guard and the coverage
# baseline gate), which has no naturally-occurring trigger in the live families.
# It defines GABY_VM_VIXL_PORT_SELFTEST to enable the fault-injection seam (a
# no-op in the family binaries). Each gaby-track scenario runs in its own process
# (a gaby crash latches the shared engine), so it is registered as one executable
# with several scenario-selecting CTest invocations.
add_executable(gaby_vm_vixl_asm_harness_selftest
  harness/two_track_harness_selftest.cc)
gaby_vm_vixl_asm_link_island(gaby_vm_vixl_asm_harness_selftest)
foreach(_scenario cache decoder reference baseline)
  add_test(NAME vixl_asm_harness_selftest_${_scenario}
    COMMAND gaby_vm_vixl_asm_harness_selftest ${_scenario})
  set_tests_properties(vixl_asm_harness_selftest_${_scenario}
    PROPERTIES TIMEOUT 60)
endforeach()

# Live-assemble vixl_port families. Each exe #includes one upstream test .cc
# under the two-track macros and runs every TEST() body live on both gaby tracks.
# These are THE vixl_port suite — they replace the frozen-fixture replay families
# that previously lived under test/vixl_port/.
function(gaby_vm_add_vixl_port_family name source)
  add_executable(${name} ${source})
  gaby_vm_vixl_asm_link_island(${name})
  add_test(NAME ${name} COMMAND ${name})
  # Runaway backstop: each body is bounded by the reference-sim instruction cap
  # and the host watchdog, but a regression that wedged the harness should fail
  # loudly rather than hang CI. Generous vs the expected few-second run.
  set_tests_properties(${name} PROPERTIES TIMEOUT 240)
endfunction()

gaby_vm_add_vixl_port_family(vixl_port_integer harness/vixl_port_integer.cc)
gaby_vm_add_vixl_port_family(vixl_port_fp harness/vixl_port_fp.cc)
gaby_vm_add_vixl_port_family(vixl_port_neon harness/vixl_port_neon.cc)
