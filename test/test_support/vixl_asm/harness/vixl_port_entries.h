// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Thin, dependency-free entry points for the live-assemble vixl_port families.
// Each returns 0 iff its family is green (same verdict the family executable's
// main() returns). The point of this header is the *absence* of includes: a
// non-CTest driver (the iOS XCTest runner) can call a family without pulling in
// any VIXL or harness header — it just links the family library and calls the
// entry. The entry is defined in the same translation unit as the family's
// registered TEST() bodies, so referencing it pulls those registrations in
// (no -force_load needed).

#ifndef GABY_VM_TEST_VIXL_ASM_VIXL_PORT_ENTRIES_H_
#define GABY_VM_TEST_VIXL_ASM_VIXL_PORT_ENTRIES_H_

namespace gaby_vm {
namespace ios_runner {

int run_vixl_port_integer();
// run_vixl_port_fp() / run_vixl_port_neon() land with their slices.

}  // namespace ios_runner
}  // namespace gaby_vm

#endif  // GABY_VM_TEST_VIXL_ASM_VIXL_PORT_ENTRIES_H_
