// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#include <cstdint>
#include <cstdio>

#include "cpu-features.h"

#include "aarch64/decoder-aarch64.h"
#include "aarch64/simulator-aarch64.h"

// Smoke test for the imported VIXL AArch64 simulator. Builds a Decoder, builds
// a Simulator on top of it (which exercises the constructors of every imported
// subsystem: decoder graph, disassembler, debugger, CPU-features auditor),
// then runs a single NOP instruction through the imported decode → visit →
// leaf path. Failure here is by abort/assertion; success is exit 0.

namespace {

constexpr uint32_t kNopEncoding = 0xd503201fU;  // AArch64 NOP

}  // namespace

int main() {
  using vixl::CPUFeatures;
  using vixl::aarch64::Decoder;
  using vixl::aarch64::Instruction;
  using vixl::aarch64::Simulator;

  // The Instruction class is a zero-byte wrapper over an instruction word, so
  // reinterpret_cast over a 4-byte-aligned buffer is the documented way to
  // construct one without going through the assembler.
  alignas(4) uint32_t code = kNopEncoding;

  Decoder decoder;
  Simulator sim(&decoder, stdout);

  // Configure the auditor with the full feature set so the auditor assertion
  // inside ExecuteInstruction() (see simulator-aarch64.h around the
  // VIXL_CHECK(cpu_features_auditor_.InstructionIsAvailable()) line) does
  // not reject legitimate baseline instructions.
  sim.SetCPUFeatures(CPUFeatures::All());

  const Instruction* pc = reinterpret_cast<const Instruction*>(&code);
  sim.WritePc(pc);
  sim.ExecuteInstruction();

  std::printf("simulator_smoke: ran one NOP at %p (decoded + dispatched OK)\n",
              static_cast<const void*>(pc));
  return 0;
}
