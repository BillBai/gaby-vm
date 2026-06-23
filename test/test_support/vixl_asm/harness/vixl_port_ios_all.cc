// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// iOS-runner-only aggregate entry. The host CTest path runs each vixl_port
// family as its own executable (one family per process, the design the harness
// assumes). The iOS runner links all three family libraries into one XCTest
// bundle — a single process whose global vixl::Test list holds every family's
// registered bodies. Calling a per-family entry there would re-walk that whole
// combined list, and running several entries in one process corrupts the
// shared, process-lifetime engines. So the iOS runner instead does ONE walk
// over the combined set under the summed "ios_runner_all" baseline.

#include "gaby_two_track_macros.h"
#include "gaby_two_track_main.h"
#include "vixl_port_entries.h"

int gaby_vm::ios_runner::run_vixl_port_all() {
  // Anchor the three family translation units so their TEST() registrations
  // (file-scope constructors) are pulled from their static libraries into any
  // binary that links this one. We take each family entry's address rather than
  // calling it — each per-family entry would itself walk the whole registered
  // set. The actual run happens once, below.
  //
  // The address is written through a VOLATILE sink so the optimiser cannot drop
  // the reference: a store to a volatile object is an observable side effect
  // the compiler MUST emit, which materialises the symbol reference and forces
  // the linker to pull the archive member. The sink must be `void* volatile` (a
  // volatile pointer), NOT `volatile void*` (a pointer to volatile void): the
  // latter leaves the pointer itself non-volatile, so the stores are ordinary
  // and get elided under -O2/LTO — which left the families unlinked and the
  // suite reporting zero registered TESTs in a Release (e.g. on-device) build.
  void* volatile anchor;
  anchor = reinterpret_cast<void*>(&gaby_vm::ios_runner::run_vixl_port_integer);
  anchor = reinterpret_cast<void*>(&gaby_vm::ios_runner::run_vixl_port_fp);
  anchor = reinterpret_cast<void*>(&gaby_vm::ios_runner::run_vixl_port_neon);
  (void)anchor;

  return gaby_vm::vixl_port_live::RunRegisteredTests("ios_runner_all");
}
