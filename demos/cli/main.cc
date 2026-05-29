// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// gaby-vm CLI demo — canonical embedding pattern.
//
// Loads a small hand-assembled guest function, registers it with the
// PredecodeCache, runs it through Simulator::RunFrom, and installs a branch
// hook that recognises the guest's call to a host function and dispatches it
// via plain C ABI. This is the end-to-end reference example for the
// branch-hook-api change (openspec/changes/branch-hook-api/).
//
// What runs.
//   Guest function (4 instructions + a label):
//
//     [0]  mov x19, lr              ; save outer LR (the null-LR sentinel)
//     [4]  bl  +12                  ; target = entry + 16 = host_marker
//     [8]  mov lr,  x19             ; restore outer LR
//     [12] ret                      ; terminates (LR = null sentinel)
//     [16] <host_marker>            ; never executed; address only
//
//   Host function:
//
//     extern "C" int host_add(int a, int b) { return a + b; }
//
// The simulator never decodes host_marker bytes: when the BL fires the branch
// hook, the hook recognises target_pc == host_marker, reads the AArch64 C-ABI
// argument registers (X0 = a, X1 = b), calls host_add through a plain C
// function pointer, writes the result back to X0, and returns the guest LR
// (the post-BL return address — the branch hook returns a bare uintptr_t next
// PC). The simulator then commits PC to LR — the `mov lr, x19` that follows
// the BL — and the guest continues. The trailing RET reads X19 back into LR,
// sees the null-LR sentinel, and terminates.
//
// The same pattern scales to any host symbol the guest patch wants to
// invoke: keep a small range-keyed dispatch table inside the hook body
// (typically "is this PC inside any of my loaded guest Mach-O ranges?") and
// fall back to FFI if not.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <ostream>
#include <string_view>
#include <vector>

#include "gaby_vm/gaby_vm.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

namespace {

constexpr std::string_view kProgName = "gaby-vm";

// -------- The host function the guest will call ------------------------------

extern "C" int host_add(int a, int b) { return a + b; }

// -------- Embedding boilerplate ----------------------------------------------

// Embedder-owned context the branch hook carries opaquely through user_data.
// Real embedders typically hold a table of loaded guest Mach-O ranges plus a
// fall-back dispatch path for host symbols; for the demo a single recognised
// "host marker" address is enough to illustrate the call pattern.
struct EmbedderContext {
  uintptr_t host_marker = 0;
};

// The hook body. Two branch families end up here in the demo:
//   1) The guest's `BL host_marker` — target_pc == host_marker. Dispatch the
//      host function via C ABI: read X0/X1 as the two int args, call
//      host_add, write the int32 result back into X0, return LR so the
//      simulator continues at the post-BL return address.
//   2) The guest's terminating RET — target_pc == 0 (the null-LR sentinel
//      the simulator initialises into LR). Return identity; the simulator
//      will commit PC to 0 and IsSimulationFinished terminates the run.
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

// -------- Demo run -----------------------------------------------------------

int RunDemo() {
  // Hand-assembled guest function — encodings verified at authorship time
  // with an external assembler (see test/simulator_correctness.cc for the
  // recipe). The runtime depends on no assembler.
  alignas(uint32_t) static const uint32_t kGuest[] = {
      0xaa1e03f3,  // [0]  mov x19, lr
      0x94000003,  // [4]  bl  #+12         (-> entry + 16 = host_marker)
      0xaa1303fe,  // [8]  mov lr,  x19
      0xd65f03c0,  // [12] ret
      0x00000000,  // [16] host_marker (data, never decoded; marker address)
  };

  // Register only the executable range — entry+0 through entry+12 (4
  // instructions). The marker word at entry+16 is data, not code; the BL's
  // target is intercepted before the simulator ever tries to decode it.
  const uintptr_t entry = reinterpret_cast<uintptr_t>(kGuest);
  const uintptr_t host_marker = entry + 16;

  gaby_vm::PredecodeCache cache;
  const auto status = cache.RegisterCodeRange(kGuest, /*size_bytes=*/16);
  if (status != gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    std::fprintf(stderr, "gaby-vm demo: RegisterCodeRange failed\n");
    return 1;
  }

  std::vector<uint8_t> stack(gaby_vm::Simulator::kMinStackSize);
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  EmbedderContext ctx{host_marker};
  sim.SetBranchHook(DemoBranchHook, &ctx);

  // Seed the AArch64 C-ABI argument registers for host_add(40, 2).
  const int a = 40;
  const int b = 2;
  sim.Write(gaby_vm::GpRegister::X0, static_cast<uint64_t>(a));
  sim.Write(gaby_vm::GpRegister::X1, static_cast<uint64_t>(b));

  // Run. The guest BLs into the host marker; the hook dispatches; the guest
  // returns; the trailing RET hits the null-LR sentinel and the run
  // terminates.
  sim.RunFrom(entry);

  const int result = static_cast<int>(
      static_cast<uint32_t>(sim.Read(gaby_vm::GpRegister::X0)));
  std::printf("host_add(%d, %d) = %d\n", a, b, result);
  return (result == a + b) ? 0 : 1;
}

// -------- argv handling ------------------------------------------------------

void PrintVersion() {
  std::cout << kProgName << " " << gaby_vm::version() << "\n";
}

void PrintUsage(std::ostream& os) {
  os << "Usage: " << kProgName << " [OPTIONS]\n"
     << "\n"
     << "Options:\n"
     << "  -v, --version    Print version and exit\n"
     << "  -h, --help       Print this help and exit\n"
     << "\n"
     << "With no options, runs the embedding demo: a hand-assembled guest\n"
     << "function that calls a host C function via the branch hook. Prints\n"
     << "the deterministic result and exits 0 on success.\n";
}

enum class Mode { kDemo, kVersion, kHelp, kUnknown };

Mode ParseArgs(int argc, char** argv) {
  if (argc <= 1) {
    return Mode::kDemo;
  }
  if (argc > 2) {
    return Mode::kUnknown;
  }
  std::string_view arg = argv[1];
  if (arg == "-v" || arg == "--version") {
    return Mode::kVersion;
  }
  if (arg == "-h" || arg == "--help") {
    return Mode::kHelp;
  }
  return Mode::kUnknown;
}

}  // namespace

int main(int argc, char** argv) {
  switch (ParseArgs(argc, argv)) {
    case Mode::kDemo:
      PrintVersion();
      return RunDemo();
    case Mode::kVersion:
      PrintVersion();
      return 0;
    case Mode::kHelp:
      PrintUsage(std::cout);
      return 0;
    case Mode::kUnknown:
      PrintUsage(std::cerr);
      return 2;
  }
  return 0;
}
