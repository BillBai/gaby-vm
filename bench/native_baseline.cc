#include "native_runner.h"
#include "workloads/mixed_workload_data.h"

int main(int argc, char* argv[]) {
  return gaby_vm_bench::RunNativeBaseline(
      "mixed",
      "Execute the mixed workload natively on the host arm64 CPU and report "
      "throughput.",
      gaby_vm_bench::kMixedWorkloadGeneratorTag,
      gaby_vm_bench::kMixedWorkloadInstructions,
      gaby_vm_bench::kMixedWorkloadStaticWordCount,
      gaby_vm_bench::kMixedWorkloadDynamicInstructionsPerIteration,
      argc,
      argv);
}
