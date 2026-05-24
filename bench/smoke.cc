#include "runner.h"
#include "workloads/smoke_workload_data.h"

int main(int argc, char* argv[]) {
  return gaby_vm_bench::
      RunBenchmark("smoke",
                   "Drive the imported VIXL AArch64 simulator over the "
                   "llvm-mc-assembled smoke workload (32 instructions).",
                   gaby_vm_bench::kSmokeWorkloadGeneratorTag,
                   gaby_vm_bench::kSmokeWorkloadInstructions,
                   gaby_vm_bench::kSmokeWorkloadStaticWordCount,
                   gaby_vm_bench::kSmokeWorkloadDynamicInstructionsPerIteration,
                   argc,
                   argv);
}
