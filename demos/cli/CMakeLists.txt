add_executable(gaby_vm_cli main.cc)
set_target_properties(gaby_vm_cli PROPERTIES OUTPUT_NAME gaby-vm)
target_link_libraries(gaby_vm_cli PRIVATE gaby_vm::gaby_vm)
gaby_vm_apply_compile_flags(gaby_vm_cli)
