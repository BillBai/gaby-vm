#ifndef GABY_VM_BENCH_NATIVE_RUNNER_H_
#define GABY_VM_BENCH_NATIVE_RUNNER_H_

#include <cstddef>
#include <cstdint>

namespace gaby_vm_bench {

// Runs the SAME committed workload bytes the simulator harness drives
// (bench_smoke / bench_baseline), but natively on the host CPU: the bytes are
// copied into executable memory and called as a function. This yields a
// "relative to native" reference for the decoder- and cache-mode numbers.
//
// Output uses the same `key: value` format as RunBenchmark, with `mode:
// native`, so native, decoder, and cache observations compose in a single log.
//
// Preconditions (enforced by bench/CMakeLists.txt, which only builds the
// native baseline targets when they hold):
//   * Host ISA is arm64 — the workload is guest AArch64 code executed directly,
//     so guest ISA must equal host ISA.
//   * The committed workload is a self-contained, AAPCS-balanced function:
//     all memory access stays on the stack, all branches stay in-code, and it
//     returns via RET. Both committed workloads satisfy this (the smoke .s by
//     construction; the mixed workload because upstream BenchCodeGenerator
//     keeps every access stack-relative via x28 and every branch in-code).
//
// Returns a process exit status: 0 on success, 2 on a usage error.
int RunNativeBaseline(const char* workload_name,
                      const char* workload_description,
                      const char* generator_tag,
                      const std::uint32_t* code,
                      std::size_t static_word_count,
                      std::uint64_t dynamic_insns_per_iter,
                      int argc,
                      char* argv[]);

}  // namespace gaby_vm_bench

#endif  // GABY_VM_BENCH_NATIVE_RUNNER_H_
