// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef GABY_VM_VERSION_H_
#define GABY_VM_VERSION_H_

namespace gaby_vm {

// Returns a NUL-terminated, statically-allocated build-time version string
// (e.g. "0.0.1"). Never returns nullptr; the pointer is valid for the
// process lifetime.
const char* version();

}  // namespace gaby_vm

#endif  // GABY_VM_VERSION_H_
