// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// ODR / link smoke gate (migration step 3). Links the test-only assembler
// island against gaby_vm.a, assembles `Mov(x0, 1)` + a terminating `Br(xzr)`,
// finalizes, and runs the result on a VIXL reference Simulator. It proves:
//
//   * the island + gaby_vm.a link with NO duplicate shared `vixl::` symbol and
//     NO undefined SVE symbol (the link itself is the proof),
//   * the gaby-authored ExecuteMemory stub resolves the only test-infra symbol
//     pulled from the deliberately-omitted test-utils.cc,
//   * the macro-assembler-aarch64.h:41 -> "simulator-aarch64.h" coupling
//     resolves to gaby's imported (dual-track) simulator and compiles/links.
//
// Kept as a permanent linkage canary, not just a one-shot gate: if a future
// edit reintroduces a duplicate definition or an unresolved assembler symbol,
// this is the cheapest test that fails.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "aarch64/macro-assembler-aarch64.h"
#include "aarch64/simulator-aarch64.h"

using vixl::CPUFeatures;
using vixl::aarch64::Decoder;
using vixl::aarch64::Instruction;
using vixl::aarch64::MacroAssembler;
using vixl::aarch64::Simulator;
using vixl::aarch64::x0;

// `br xzr` — branch to the zero register (address 0 = the end-of-simulation
// sentinel), so RunFrom terminates without depending on LR. Appended as a raw
// word rather than via MacroAssembler::Br(xzr), which asserts `!xn.IsZero()`
// under VIXL_DEBUG. This mirrors vixl_port_runner.cc's terminator.
constexpr uint32_t kBrXzr = 0xd61f03e0u;

int main() {
  MacroAssembler masm;
  masm.SetGenerateSimulatorCode(true);
  masm.Mov(x0, 1);
  masm.FinalizeCode();

  // Assembled bytes are ordinary heap data fed to the decoder, never executed
  // on the host (no-JIT / no-RWX). Copy the body out and append the terminator.
  std::vector<uint32_t>
      program(masm.GetBuffer()->GetStartAddress<const uint32_t*>(),
              masm.GetBuffer()->GetEndAddress<const uint32_t*>());
  program.push_back(kBrXzr);

  Decoder decoder;
  Simulator simulator(&decoder);
  simulator.SetCPUFeatures(CPUFeatures::All());
  simulator.RunFrom(reinterpret_cast<const Instruction*>(program.data()));

  const int64_t x0v = simulator.ReadXRegister(0);
  if (x0v != 1) {
    std::fprintf(stderr,
                 "vixl_asm island smoke FAIL: x0 = %lld, want 1\n",
                 static_cast<long long>(x0v));
    return 1;
  }
  std::printf(
      "vixl_asm island smoke OK: assembled + ran Mov(x0,1) on the reference "
      "simulator, x0 = 1\n");
  return 0;
}
