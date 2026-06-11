// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Live-assemble FP family driver. Same shape as vixl_port_integer_live.cc, over
// the upstream VIXL floating-point test bodies.
#include "gaby_two_track_macros.h"
#include "gaby_two_track_main.h"

#include "aarch64/test-assembler-fp-aarch64.cc"  // upstream FP bodies (verbatim)

int main() { return gaby_vm::vixl_port_live::RunRegisteredTests("fp"); }
