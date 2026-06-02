#include "native_runner.h"
#include "workloads/smoke_workload_data.h"

int main(int argc, char* argv[]) {
  return gaby_vm_bench::RunNativeBaseline(
      "smoke",
      "Execute the smoke workload natively on the host arm64 CPU and report "
      "throughput.",
      gaby_vm_bench::kSmokeWorkloadGeneratorTag,
      gaby_vm_bench::kSmokeWorkloadInstructions,
      gaby_vm_bench::kSmokeWorkloadStaticWordCount,
      gaby_vm_bench::kSmokeWorkloadDynamicInstructionsPerIteration,
      argc,
      argv);
}
