#ifndef GABY_VM_BENCH_WORKLOADS_BUSINESS_HASH_WORKLOAD_DATA_H_
#define GABY_VM_BENCH_WORKLOADS_BUSINESS_HASH_WORKLOAD_DATA_H_

#include <cstddef>
#include <cstdint>

// Business-logic microkernel "hash" — committed bytes, regenerated
// offline by bench/workloads/business/gen_business_workloads.sh from
// bench/workloads/business/hash.c. Do not edit by hand. The dynamic
// instruction count and expected-x0 oracle live in oracle_data.h.

namespace gaby_vm_bench {

inline constexpr std::uint32_t kHashWorkloadInstructions[] = {
    0xd28464a0,
    0xd28f82a9,
    0xd28fe5aa,
    0xd29029eb,
    0xd29199ad,
    0xf2b08440,
    0xf2afe949,
    0xf2a992aa,
    0xf2beeceb,
    0xf2bdaaad,
    0xf2d39c80,
    0xf2cf3729,
    0xf2de85aa,
    0xf2cf6fcb,
    0xd280366c,
    0xf2d5faed,
    0x2a1f03e8,
    0xf2f97e40,
    0xf2f3c6e9,
    0xf2eb0a2a,
    0xf2e280ab,
    0xf2c0200c,
    0xf2ffea2d,
    0x5280200e,
    0x9b0a2d29,
    0x710005ce,
    0xca49e00f,
    0x9b0c7def,
    0xca4f85ef,
    0x9b0d7def,
    0xca4f85e0,
    0x54ffff21,
    0x11000508,
    0x7100411f,
    0x54fffea1,
    0xd65f03c0,
};

inline constexpr std::size_t kHashWorkloadStaticWordCount =
    sizeof(kHashWorkloadInstructions) / sizeof(kHashWorkloadInstructions[0]);

inline constexpr char kHashWorkloadGeneratorTag[] =
    "Homebrew clang version 22.1.5; flags=O2/general-regs-only/no-jump-tables; source_sha256=521b4fafc36e";

}  // namespace gaby_vm_bench

#endif
