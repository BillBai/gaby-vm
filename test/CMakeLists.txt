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

# Constexpr smoke test for the 6 VectorFormat helpers promoted to constexpr
# inline by the openspec change `neon-format-helpers-constexpr-inline`. The TU
# carries a set of static_asserts; if the promotion is intact, the test
# compiles. Runtime body is trivial. Uses the same privileged-build pattern as
# simulator_smoke (PRIVATE src/ include + VIXL_* defines) so it can reach the
# imported `aarch64/instructions-aarch64.h` directly.
add_executable(instructions_aarch64_constexpr_smoke_test
  instructions_aarch64_constexpr_smoke_test.cc)
target_link_libraries(instructions_aarch64_constexpr_smoke_test
  PRIVATE gaby_vm::gaby_vm)
target_include_directories(instructions_aarch64_constexpr_smoke_test
  PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(instructions_aarch64_constexpr_smoke_test PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)
gaby_vm_apply_compile_flags(instructions_aarch64_constexpr_smoke_test)

add_test(NAME instructions_aarch64_constexpr_smoke
  COMMAND instructions_aarch64_constexpr_smoke_test)

# Simulator correctness: the dual-path harness. Drives hand-encoded AArch64
# sequences through gaby_vm::Simulator::RunFrom (cache track) and DebugRunFrom
# (decoder track) and asserts both reach the precomputed expected state. It
# consumes only gaby_vm:: public types, so — unlike simulator_smoke — it needs
# no privileged build pattern: no PRIVATE src/ include, no VIXL_* defines.
add_executable(simulator_correctness simulator_correctness.cc)
target_link_libraries(simulator_correctness PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(simulator_correctness)

add_test(NAME simulator_correctness COMMAND simulator_correctness)

# Re-entrancy + dual-track termination: drives the gaby_vm::Simulator public API
# only (no imported VIXL headers), so it needs no privileged build pattern —
# just the public include directory that linking gaby_vm::gaby_vm provides.
add_executable(reentrancy_test reentrancy_test.cc)
target_link_libraries(reentrancy_test PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(reentrancy_test)

add_test(NAME reentrancy COMMAND reentrancy_test)

# ShadowRunner self-test: verifies the oracle reports zero divergence on a
# matching run AND — via a deliberately injected cache-track defect — that a
# divergence is actually detected. Public-API only; no privileged build.
add_executable(shadow_runner_test shadow_runner_test.cc)
target_link_libraries(shadow_runner_test PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(shadow_runner_test)

add_test(NAME shadow_runner COMMAND shadow_runner_test)

# Workload shadow acceptance: runs ShadowRunner over every committed benchmark
# workload and asserts zero divergence (design doc 4.5 acceptance #3). The
# PRIVATE bench/ include is only for the pure-data workload headers under
# bench/workloads/ — it is not the privileged VIXL build pattern.
add_executable(workload_shadow_test workload_shadow_test.cc)
target_link_libraries(workload_shadow_test PRIVATE gaby_vm::gaby_vm)
target_include_directories(workload_shadow_test PRIVATE ${PROJECT_SOURCE_DIR}/bench)
gaby_vm_apply_compile_flags(workload_shadow_test)

add_test(NAME workload_shadow COMMAND workload_shadow_test)

# Typed register-I/O acceptance: covers the simulator-register-io-api change's
# new public surface — enum-keyed Read/Write, RegisterFile snapshot, ReadAll /
# WriteAll, and the std::span<RegisterWrite> batch entry point. Public-API
# only; no privileged build.
add_executable(typed_register_io_test typed_register_io_test.cc)
target_link_libraries(typed_register_io_test PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(typed_register_io_test)

add_test(NAME typed_register_io COMMAND typed_register_io_test)

# Typed register-I/O fail-fast: exercises the span-write abort path on a
# deliberately out-of-range GpRegister value. The test program forks a child
# that triggers the abort and asserts in the parent that the child died from
# a signal, so the parent exits 0/1 the normal way — no WILL_FAIL needed
# (and that property doesn't invert SIGABRT outcomes anyway).
add_executable(typed_register_io_abort_test typed_register_io_abort_test.cc)
target_link_libraries(typed_register_io_abort_test PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(typed_register_io_abort_test)

add_test(NAME typed_register_io_abort COMMAND typed_register_io_abort_test)

# Simulator constructor stack-size contract: the constructor MUST abort when
# stack_size is below Simulator::kMinStackSize. Same fork-and-check-for-signal
# pattern as typed_register_io_abort_test, for the same reason (WILL_FAIL does
# not invert SIGABRT outcomes). Also verifies the boundary case
# stack_size == kMinStackSize constructs successfully.
add_executable(simulator_constructor_stack_test simulator_constructor_stack_test.cc)
target_link_libraries(simulator_constructor_stack_test PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(simulator_constructor_stack_test)

add_test(NAME simulator_constructor_stack COMMAND simulator_constructor_stack_test)
