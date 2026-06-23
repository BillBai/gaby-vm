// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Live-assemble NEON family driver. Same shape as vixl_port_integer_live.cc,
// over the upstream VIXL NEON test bodies. SVE bodies are not included (no gaby
// SVE leaf); only the assembler symbols are satisfied by the island.
#include "gaby_two_track_macros.h"
#include "gaby_two_track_main.h"
#include "vixl_port_entries.h"

#include "aarch64/test-assembler-neon-aarch64.cc"  // upstream NEON bodies (verbatim)

int gaby_vm::ios_runner::run_vixl_port_neon() {
  return gaby_vm::vixl_port_live::RunRegisteredTests("neon");
}

int main() { return gaby_vm::ios_runner::run_vixl_port_neon(); }
