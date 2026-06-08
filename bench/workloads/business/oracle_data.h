#ifndef GABY_VM_BENCH_WORKLOADS_BUSINESS_ORACLE_DATA_H_
#define GABY_VM_BENCH_WORKLOADS_BUSINESS_ORACLE_DATA_H_

#include <cstdint>

// Runtime-captured oracle constants for the business-logic microkernels.
//
// HAND-MAINTAINED — gen_business_workloads.sh never writes this file. These two
// per-kernel values need the simulator at runtime, so they are produced by
// `bench_business --verify` (which prints a copy-pasteable line per kernel) and
// pasted here by hand. Keeping them out of the generated *_workload_data.h is
// deliberate: a workload regeneration regenerates only the machine-derived
// bytes and can never silently reset these.
//
//   *DynamicInstructionsPerIteration — instructions executed per RunFrom pass,
//       used only for the derived ns/insn metric (the primary
//       iterations_per_second figure does not depend on it).
//   *ExpectedResult — the x0 a correct run returns; the cache and decoder
//   tracks
//       must both reproduce it (checked by --verify).
//
// Re-run `bench_business --verify` after any kernel source or simulator-leaf
// change and update the values below if it reports a mismatch.

namespace gaby_vm_bench {

inline constexpr std::uint64_t kParseWorkloadDynamicInstructionsPerIteration =
    156172;
inline constexpr std::uint64_t kParseWorkloadExpectedResult =
    0xc196940487fb10c0ull;

inline constexpr std::uint64_t kHashWorkloadDynamicInstructionsPerIteration =
    32856;
inline constexpr std::uint64_t kHashWorkloadExpectedResult =
    0x76dd61c78f2a67e3ull;

inline constexpr std::uint64_t kStructWorkloadDynamicInstructionsPerIteration =
    47827;
inline constexpr std::uint64_t kStructWorkloadExpectedResult =
    0x000005cf4397d892ull;

inline constexpr std::uint64_t kFsmWorkloadDynamicInstructionsPerIteration =
    667366;
inline constexpr std::uint64_t kFsmWorkloadExpectedResult =
    0x72279a557be93af8ull;

}  // namespace gaby_vm_bench

#endif  // GABY_VM_BENCH_WORKLOADS_BUSINESS_ORACLE_DATA_H_
