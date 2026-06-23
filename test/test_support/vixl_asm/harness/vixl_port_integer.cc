// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Live-assemble integer family driver. Pulls in the two-track macros, then the
// upstream VIXL integer test bodies VERBATIM (its no-guard self-include of
// test-assembler-aarch64.h was stripped at copy time), then the family main.
// Each TEST() assembles live and runs on both gaby tracks under the absolute +
// differential oracles. Unlike the frozen fixtures, load/store / ADR / literal
// bodies run for real here.
#include "gaby_two_track_macros.h"
#include "gaby_two_track_main.h"
#include "vixl_port_entries.h"

#include "aarch64/test-assembler-aarch64.cc"  // upstream bodies (verbatim)

// Callable entry: same TU as the registered TEST() bodies above, so a consumer
// that references this symbol pulls the registrations in. The family executable
// and the iOS XCTest runner both reach the suite through this one function.
int gaby_vm::ios_runner::run_vixl_port_integer() {
  return gaby_vm::vixl_port_live::RunRegisteredTests("integer");
}

int main() { return gaby_vm::ios_runner::run_vixl_port_integer(); }
