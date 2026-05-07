# Generate version.cc from the template so PROJECT_VERSION is the single
# source of truth at build time.
configure_file(version.cc.in ${CMAKE_CURRENT_BINARY_DIR}/version.cc @ONLY)

add_library(gaby_vm STATIC)
add_library(gaby_vm::gaby_vm ALIAS gaby_vm)

target_sources(gaby_vm PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.cc)

target_include_directories(gaby_vm
  PUBLIC  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_compile_features(gaby_vm PUBLIC cxx_std_17)

gaby_vm_apply_compile_flags(gaby_vm)
