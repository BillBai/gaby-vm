# Helper for applying the project's standard warning flags to a target.
# GCC, Clang, and AppleClang only — Windows is out of scope per AGENTS.md,
# so MSVC is intentionally not handled.

function(gaby_vm_apply_compile_flags target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic)
  endif()
endfunction()
