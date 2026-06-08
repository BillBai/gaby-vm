#ifndef GABY_VM_BENCH_WORKLOADS_BUSINESS_STRUCT_WORKLOAD_DATA_H_
#define GABY_VM_BENCH_WORKLOADS_BUSINESS_STRUCT_WORKLOAD_DATA_H_

#include <cstddef>
#include <cstdint>

// Business-logic microkernel "struct" — committed bytes, regenerated
// offline by bench/workloads/business/gen_business_workloads.sh from
// bench/workloads/business/struct.c. Do not edit by hand. The dynamic
// instruction count and expected-x0 oracle live in oracle_data.h.

namespace gaby_vm_bench {

inline constexpr std::uint32_t kStructWorkloadInstructions[] = {
    0xf81f0ffd,
    0xd12003ff,
    0xd28bcf2a,
    0xd28fe5ab,
    0xd29029ec,
    0xf2a3678a,
    0xf2a992ab,
    0xf2beecec,
    0x910003e9,
    0xf2cf49aa,
    0xf2de85ab,
    0xf2cf6fcc,
    0xaa1f03e8,
    0x91002129,
    0xf2f6a52a,
    0xf2eb0a2b,
    0xf2e280ac,
    0x9b0b314a,
    0xd360fd4d,
    0x531c794e,
    0x293f3528,
    0x91000508,
    0xf102011f,
    0x2882392a,
    0x54ffff21,
    0x910003e9,
    0x529d32aa,
    0xaa1f03e0,
    0x2a1f03e8,
    0x91002129,
    0x72ab7a2a,
    0x14000004,
    0x11000508,
    0x7100611f,
    0x54000340,
    0x5280100b,
    0xaa0903ec,
    0x1400000c,
    0x0b0f05ef,
    0x0b0e01ee,
    0x4a0a01cf,
    0x721e01bf,
    0x9100418d,
    0x1a8f01ce,
    0xf100056b,
    0xb81fc18e,
    0x8b0e0000,
    0xaa0d03ec,
    0x54fffe00,
    0x2940358f,
    0xb85fc18e,
    0x3707fe6d,
    0x370800ad,
    0xb85f8190,
    0x4a0e01ee,
    0x0b0e020e,
    0x17fffff0,
    0x6b0f01ce,
    0x5a8e85ce,
    0x17ffffed,
    0x912003ff,
    0xf84107fd,
    0xd65f03c0,
};

inline constexpr std::size_t kStructWorkloadStaticWordCount =
    sizeof(kStructWorkloadInstructions) / sizeof(kStructWorkloadInstructions[0]);

inline constexpr char kStructWorkloadGeneratorTag[] =
    "Homebrew clang version 22.1.5; flags=O2/general-regs-only/no-jump-tables; source_sha256=f00dfddcb231";

}  // namespace gaby_vm_bench

#endif
