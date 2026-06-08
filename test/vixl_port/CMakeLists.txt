# Ported VIXL simulator tests (replay side). Self-contained: consumes only the
# gaby_vm public API plus the shared test/embedding_stack.h, so it needs no
# privileged VIXL build pattern and does NOT depend on ../vixl. The fixtures it
# replays are generated and committed under generated/ by the authorship-time
# extraction tool (tools/vixl_test_extract, built only behind
# GABY_VM_BUILD_VIXL_EXTRACT). See docs/testing.md.

add_executable(vixl_port_integer
  vixl_port_integer.cc
  vixl_port_runner.cc)
target_link_libraries(vixl_port_integer PRIVATE gaby_vm::gaby_vm)
target_include_directories(vixl_port_integer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}     # vixl_port_fixture.h, vixl_port_runner.h, generated/
  ${PROJECT_SOURCE_DIR}/test)     # embedding_stack.h
gaby_vm_apply_compile_flags(vixl_port_integer)
add_test(NAME vixl_port_integer COMMAND vixl_port_integer)
# Runaway backstop: a regression that broke termination would otherwise hang
# (the runner uses unbounded RunFrom for speed). CTest turns the overrun into a
# reported failure instead of a silent CI hang. Generous vs the ~1s normal run.
set_tests_properties(vixl_port_integer PROPERTIES TIMEOUT 120)
