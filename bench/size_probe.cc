// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// size_probe — minimal public-API link unit for the CI binary-size report.
//
// This is NOT a benchmark or a test. Its only purpose is to pull the gaby_vm
// public API surface (PredecodeCache + Simulator + register I/O) into one small
// executable so ci/size-report.sh can strip it and measure the footprint an
// embedder actually pays. Keep it minimal: touch the public entry points an
// embedder uses, nothing more. It links ONLY the public gaby_vm::gaby_vm
// library (no private src include, no VIXL defines).

#include <cstdint>
#include <vector>

#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

int main() {
  // mov x0, #42 ; ret  — exercises register I/O + the run path. RET reads the
  // null-LR sentinel the simulator seeds and terminates (same pattern as the
  // CLI demo).
  alignas(uint32_t) static const uint32_t kCode[] = {
      0xd2800540,  // mov x0, #42
      0xd65f03c0,  // ret
  };

  gaby_vm::PredecodeCache cache;
  if (cache.RegisterCodeRange(kCode, sizeof(kCode)) !=
      gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    return 1;
  }

  std::vector<uint8_t> stack(gaby_vm::Simulator::kMinStackSize);
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
  sim.RunFrom(reinterpret_cast<uintptr_t>(kCode));

  return sim.Read(gaby_vm::GpRegister::X0) == 42 ? 0 : 2;
}
