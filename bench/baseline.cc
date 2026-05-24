#include "runner.h"
#include "workloads/mixed_workload_data.h"

int main(int argc, char* argv[]) {
  return gaby_vm_bench::
      RunBenchmark("mixed",
                   "Drive the imported VIXL AArch64 simulator over the mixed "
                   "workload and report throughput.",
                   gaby_vm_bench::kMixedWorkloadGeneratorTag,
                   gaby_vm_bench::kMixedWorkloadInstructions,
                   gaby_vm_bench::kMixedWorkloadStaticWordCount,
                   gaby_vm_bench::kMixedWorkloadDynamicInstructionsPerIteration,
                   argc,
                   argv);
}
