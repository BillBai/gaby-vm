#ifndef GABY_VM_BENCH_WORKLOADS_SMOKE_WORKLOAD_DATA_H_
#define GABY_VM_BENCH_WORKLOADS_SMOKE_WORKLOAD_DATA_H_

#include <cstddef>
#include <cstdint>

// Smoke workload bytes — committed source, regenerated offline by the
// llvm-mc + llvm-objcopy pipeline documented at the top of
// bench/workloads/smoke_workload.s and in bench/README.md.

namespace gaby_vm_bench {

inline constexpr std::uint32_t kSmokeWorkloadInstructions[] = {
    // Prologue.
    0xa9bf7bfd,  // stp x29, x30, [sp, #-0x10]!
    0x910003fd,  // mov x29, sp
    // Body: 7 repetitions of a 4-instruction ALU pattern.
    0x91000400,  // add x0, x0, #1
    0xca020021,  // eor x1, x1, x2
    0xaa040063,  // orr x3, x3, x4
    0xd10004a5,  // sub x5, x5, #1
    0x91000400,
    0xca020021,
    0xaa040063,
    0xd10004a5,
    0x91000400,
    0xca020021,
    0xaa040063,
    0xd10004a5,
    0x91000400,
    0xca020021,
    0xaa040063,
    0xd10004a5,
    0x91000400,
    0xca020021,
    0xaa040063,
    0xd10004a5,
    0x91000400,
    0xca020021,
    0xaa040063,
    0xd10004a5,
    0x91000400,
    0xca020021,
    0xaa040063,
    0xd10004a5,
    // Epilogue.
    0xa8c17bfd,  // ldp x29, x30, [sp], #0x10
    0xd65f03c0,  // ret
};

inline constexpr std::size_t kSmokeWorkloadStaticWordCount =
    sizeof(kSmokeWorkloadInstructions) / sizeof(kSmokeWorkloadInstructions[0]);

// Body is branch-free: every static word is executed exactly once per
// RunFrom call, so the dynamic count equals the static count by
// construction (see design.md decision 2).
inline constexpr std::uint64_t kSmokeWorkloadDynamicInstructionsPerIteration =
    kSmokeWorkloadStaticWordCount;

inline constexpr char kSmokeWorkloadGeneratorTag[] =
    "llvm-mc 22.1.5; source_sha256=4769ba17a5fe";

}  // namespace gaby_vm_bench

#endif  // GABY_VM_BENCH_WORKLOADS_SMOKE_WORKLOAD_DATA_H_
