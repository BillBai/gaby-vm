// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// gaby-vm SwiftPM demo — proves the SPM build closes and is usable on macOS.
//
// This is the macOS verification target for the `swiftpm-package` change: a
// C++ executable that depends on the `gaby_vm` SwiftPM *product* (not the
// in-tree sources) and exercises the same embedding path the real C++ embedder
// takes — `#include "gaby_vm/…"`, call `version()`, drive a guest program
// through a `gaby_vm::Simulator`. `swift run gaby_vm_spm_demo` building,
// linking, and running it end-to-end is the proof that the package is
// consumable. It is an executable target, NOT a library product, so consumers
// of the `gaby_vm` library never build it.
//
// The guest program is the canonical host_add-via-branch-hook snippet, kept in
// step with demos/cli/main.cc (the fuller CMake demo). See that file for the
// blow-by-blow of the encoding and the hook dispatch.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <vector>

#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"
#include "gaby_vm/version.h"

namespace {

// The host function the guest calls through the branch hook.
extern "C" int host_add(int a, int b) { return a + b; }

// Embedder context the branch hook carries opaquely through user_data.
struct EmbedderContext {
  uintptr_t host_marker = 0;
};

// When the guest's `BL host_marker` fires, dispatch host_add via C ABI: read
// X0/X1 as the two int args, write the result back to X0, and return LR so the
// simulator resumes at the post-BL return address. Any other branch (the
// terminating RET into the null-LR sentinel) returns identity.
uintptr_t DemoBranchHook(uintptr_t target_pc,
                         void* user_data,
                         gaby_vm::Simulator& sim) {
  auto* ctx = static_cast<EmbedderContext*>(user_data);
  if (target_pc == ctx->host_marker) {
    const uint64_t a = sim.Read(gaby_vm::GpRegister::X0);
    const uint64_t b = sim.Read(gaby_vm::GpRegister::X1);
    const int result = host_add(static_cast<int>(a), static_cast<int>(b));
    sim.Write(gaby_vm::GpRegister::X0,
              static_cast<uint64_t>(static_cast<uint32_t>(result)));
    return sim.Read(gaby_vm::GpRegister::LR);
  }
  return target_pc;
}

}  // namespace

int main() {
  std::printf("gaby_vm %s (SwiftPM demo)\n", gaby_vm::version());

  // Hand-assembled guest function — encodings verified at authorship time with
  // an external assembler; the runtime depends on no assembler.
  //   [0]  mov x19, lr     ; save outer LR (null-LR sentinel)
  //   [4]  bl  #+12        ; target = entry + 16 = host_marker
  //   [8]  mov lr,  x19    ; restore outer LR
  //   [12] ret             ; terminates (LR = null sentinel)
  //   [16] host_marker     ; data, never decoded; address only
  alignas(uint32_t) static const uint32_t kGuest[] = {
      0xaa1e03f3,  // [0]  mov x19, lr
      0x94000003,  // [4]  bl  #+12
      0xaa1303fe,  // [8]  mov lr,  x19
      0xd65f03c0,  // [12] ret
      0x00000000,  // [16] host_marker (data)
  };

  const uintptr_t entry = reinterpret_cast<uintptr_t>(kGuest);
  const uintptr_t host_marker = entry + 16;

  gaby_vm::PredecodeCache cache;
  if (cache.RegisterCodeRange(kGuest, /*size_bytes=*/16) !=
      gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    std::fprintf(stderr, "gaby-vm SPM demo: RegisterCodeRange failed\n");
    return 1;
  }

  std::vector<uint8_t> stack(gaby_vm::Simulator::kMinStackSize);
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  EmbedderContext ctx{host_marker};
  sim.SetBranchHook(DemoBranchHook, &ctx);

  const int a = 40;
  const int b = 2;
  sim.Write(gaby_vm::GpRegister::X0, static_cast<uint64_t>(a));
  sim.Write(gaby_vm::GpRegister::X1, static_cast<uint64_t>(b));

  sim.RunFrom(entry);

  const int result = static_cast<int>(
      static_cast<uint32_t>(sim.Read(gaby_vm::GpRegister::X0)));
  std::printf("guest result: host_add(%d, %d) = %d\n", a, b, result);
  return (result == a + b) ? 0 : 1;
}
