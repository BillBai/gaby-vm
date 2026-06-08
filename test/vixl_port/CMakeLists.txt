# Ported VIXL simulator tests (replay side). Self-contained: consumes only the
# gaby_vm public API plus the shared test/embedding_stack.h, so it needs no
# privileged VIXL build pattern and does NOT depend on ../vixl. The fixtures it
# replays are generated and committed under generated/ by the authorship-time
# extraction tool (tools/vixl_test_extract, built only behind
# GABY_VM_BUILD_VIXL_EXTRACT). See docs/testing.md.

# One CTest executable per instruction family, so CTest reports failures per
# family. Each links the shared replay runner and includes its committed
# generated/<family>_fixtures.inc.
function(gaby_vm_add_vixl_port_test name source)
  add_executable(${name} ${source} vixl_port_runner.cc)
  target_link_libraries(${name} PRIVATE gaby_vm::gaby_vm)
  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}    # vixl_port_fixture.h, vixl_port_runner.h, generated/
    ${PROJECT_SOURCE_DIR}/test)    # embedding_stack.h
  gaby_vm_apply_compile_flags(${name})
  add_test(NAME ${name} COMMAND ${name})
  # Runaway backstop: a regression that broke termination would otherwise hang
  # (the runner uses unbounded RunFrom for speed). CTest turns the overrun into
  # a reported failure instead of a silent CI hang; generous vs the ~1s run.
  set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

gaby_vm_add_vixl_port_test(vixl_port_integer vixl_port_integer.cc)
gaby_vm_add_vixl_port_test(vixl_port_fp vixl_port_fp.cc)
