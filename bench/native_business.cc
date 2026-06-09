// native_business — host-CPU baseline for the business-logic microkernels.
//
// Copies the SAME committed kernel bytes the simulator harness drives into
// executable memory and runs them directly on the host arm64 CPU (mode:
// native), per kernel. This is the honest denominator for the slowdown figure:
// the same business-logic machine code, on the same CPU, with no interpreter in
// the loop. Unlike the smoke baseline (a 4-IPC straight-line ALU stream that
// flatters the native side), these kernels carry the branches, dependencies,
// and load/store traffic of real business logic, so native_business / cache
// gives a slowdown that reflects the actual iOS hot-fix scenario.
//
// Mirrors native_smoke / native_baseline; adds --kernel selection. All other
// flags (--seconds) pass through to RunNativeBaseline.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "native_runner.h"
#include "workloads/business/applogic_workload_data.h"
#include "workloads/business/fsm_workload_data.h"
#include "workloads/business/hash_workload_data.h"
#include "workloads/business/oracle_data.h"
#include "workloads/business/parse_workload_data.h"
#include "workloads/business/struct_workload_data.h"

namespace {

struct Kernel {
  const char* name;
  const char* description;
  const char* tag;
  const std::uint32_t* code;
  std::size_t static_words;
  std::uint64_t dynamic_per_iter;
};

constexpr Kernel kKernels[] = {
    {"parse",
     "Execute the parse microkernel natively on the host arm64 CPU.",
     gaby_vm_bench::kParseWorkloadGeneratorTag,
     gaby_vm_bench::kParseWorkloadInstructions,
     gaby_vm_bench::kParseWorkloadStaticWordCount,
     gaby_vm_bench::kParseWorkloadDynamicInstructionsPerIteration},
    {"hash",
     "Execute the hash microkernel natively on the host arm64 CPU.",
     gaby_vm_bench::kHashWorkloadGeneratorTag,
     gaby_vm_bench::kHashWorkloadInstructions,
     gaby_vm_bench::kHashWorkloadStaticWordCount,
     gaby_vm_bench::kHashWorkloadDynamicInstructionsPerIteration},
    {"struct",
     "Execute the struct microkernel natively on the host arm64 CPU.",
     gaby_vm_bench::kStructWorkloadGeneratorTag,
     gaby_vm_bench::kStructWorkloadInstructions,
     gaby_vm_bench::kStructWorkloadStaticWordCount,
     gaby_vm_bench::kStructWorkloadDynamicInstructionsPerIteration},
    {"fsm",
     "Execute the fsm microkernel natively on the host arm64 CPU.",
     gaby_vm_bench::kFsmWorkloadGeneratorTag,
     gaby_vm_bench::kFsmWorkloadInstructions,
     gaby_vm_bench::kFsmWorkloadStaticWordCount,
     gaby_vm_bench::kFsmWorkloadDynamicInstructionsPerIteration},
    {"applogic",
     "Execute the mixed app-logic (FP+NEON) microkernel natively on the host "
     "arm64 CPU.",
     gaby_vm_bench::kApplogicWorkloadGeneratorTag,
     gaby_vm_bench::kApplogicWorkloadInstructions,
     gaby_vm_bench::kApplogicWorkloadStaticWordCount,
     gaby_vm_bench::kApplogicWorkloadDynamicInstructionsPerIteration},
};

constexpr std::size_t kKernelCount = sizeof(kKernels) / sizeof(kKernels[0]);

bool KnownKernel(const char* name) {
  for (const Kernel& k : kKernels) {
    if (std::strcmp(k.name, name) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* selected = "all";

  std::vector<char*> forward;
  forward.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--kernel") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for --kernel\n");
        return 2;
      }
      selected = argv[++i];
      continue;
    }
    forward.push_back(argv[i]);
  }

  const bool all = std::strcmp(selected, "all") == 0;
  if (!all && !KnownKernel(selected)) {
    std::fprintf(stderr,
                 "unknown kernel: %s (expected "
                 "all|parse|hash|struct|fsm|applogic)\n",
                 selected);
    return 2;
  }

  int rc = 0;
  for (std::size_t idx = 0; idx < kKernelCount; ++idx) {
    const Kernel& k = kKernels[idx];
    if (!all && std::strcmp(k.name, selected) != 0) {
      continue;
    }
    if (idx != 0 && all) {
      std::printf("\n");
    }
    const int r =
        gaby_vm_bench::RunNativeBaseline(k.name,
                                         k.description,
                                         k.tag,
                                         k.code,
                                         k.static_words,
                                         k.dynamic_per_iter,
                                         static_cast<int>(forward.size()),
                                         forward.data());
    if (r != 0) {
      rc = r;
    }
  }
  return rc;
}
