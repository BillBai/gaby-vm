// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef GABY_VM_VERSION_H_
#define GABY_VM_VERSION_H_

// Single source of truth for the project version. Both the CMake and SwiftPM
// builds consume this header directly; there is no build-time code generation.
// Bump this string to cut a release (git tag v<x.y.z>).
#define GABY_VM_VERSION_STRING "0.0.1"

namespace gaby_vm {

// Returns a NUL-terminated, statically-allocated version string
// (e.g. "0.0.1"). Never returns nullptr; the pointer is valid for the
// process lifetime. Defined inline so the header alone is the version source
// of truth, with no generated translation unit to keep in sync.
inline const char* version() { return GABY_VM_VERSION_STRING; }

}  // namespace gaby_vm

#endif  // GABY_VM_VERSION_H_
