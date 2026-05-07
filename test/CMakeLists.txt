add_executable(gaby_vm_smoke smoke.cc)
target_link_libraries(gaby_vm_smoke PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(gaby_vm_smoke)

add_test(NAME smoke COMMAND gaby_vm_smoke)
