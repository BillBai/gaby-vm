add_executable(gaby_vm_smoke smoke.cc)
target_link_libraries(gaby_vm_smoke PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(gaby_vm_smoke)

add_test(NAME smoke COMMAND gaby_vm_smoke)

# Simulator smoke: constructs a vixl::aarch64::Simulator and runs a single
# NOP through the imported decode → visit → leaf path. PRIVATE include access
# to ${PROJECT_SOURCE_DIR}/src lets the test reach VIXL headers directly,
# without exposing them in include/gaby_vm/ (see specs/aarch64-simulator/spec.md
# requirement R11 and design.md decision D6).
add_executable(simulator_smoke simulator_smoke.cc)
target_link_libraries(simulator_smoke PRIVATE gaby_vm::gaby_vm)
target_include_directories(simulator_smoke PRIVATE ${PROJECT_SOURCE_DIR}/src)
# The same VIXL build defines that gate the simulator code in gaby_vm must
# also gate the test's view of the imported headers (e.g. simulator-aarch64.h
# is wrapped in #ifdef VIXL_INCLUDE_SIMULATOR_AARCH64). PRIVATE here keeps the
# defines off any *external* consumer of gaby_vm; the test is privileged, the
# same way its target_include_directories above grants it internal access.
target_compile_definitions(simulator_smoke PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)
gaby_vm_apply_compile_flags(simulator_smoke)

add_test(NAME simulator_smoke COMMAND simulator_smoke)

# Simulator correctness: drives hand-encoded AArch64 sequences through
# Simulator::RunFrom and asserts on post-run register / memory state. Reuses
# the privileged build pattern simulator_smoke uses (PRIVATE include of src/,
# same three VIXL compile definitions); see
# openspec/changes/baseline-correctness-test/{design.md (D10), specs/aarch64-simulator/spec.md}.
add_executable(simulator_correctness simulator_correctness.cc)
target_link_libraries(simulator_correctness PRIVATE gaby_vm::gaby_vm)
target_include_directories(simulator_correctness PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(simulator_correctness PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)
gaby_vm_apply_compile_flags(simulator_correctness)

add_test(NAME simulator_correctness COMMAND simulator_correctness)
