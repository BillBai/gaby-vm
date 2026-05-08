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

# Helper for compile flags applied only to imported VIXL-derived sources.
#
# The strict project policy (-Wall -Wextra -Wpedantic) is the wrong bar for
# code that originated upstream: we want to keep meaningful signal, but we
# cannot afford to relitigate every -Wpedantic / -Wshadow / etc. complaint
# that VIXL's style legitimately produces.
#
# Apply this to a *file list* (not a target). It scopes the relaxed flags via
# set_source_files_properties so project-authored files in the same target
# stay governed by gaby_vm_apply_compile_flags() above.
#
# The -Wno-* list is empty by default and is filled in empirically as the
# import surfaces real warnings. Every -Wno-* MUST have a one-line inline
# comment naming what triggers it; see specs/aarch64-simulator/spec.md R7.
function(gaby_vm_apply_imported_compile_flags)
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    set(_imported_flags
      -Wall
      -Wextra
      # -Wno-* relaxations are added below as the import surfaces them.
      # Each line MUST name the warning class it is silencing.
      -Wno-deprecated-literal-operator # decoder-constants-aarch64.h: `operator"" _b` (C++14 syntax, deprecated in C++23)
    )
    set_source_files_properties(${ARGN}
      PROPERTIES
        COMPILE_OPTIONS "${_imported_flags}")
  endif()
endfunction()
