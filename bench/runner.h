#ifndef GABY_VM_BENCH_RUNNER_H_
#define GABY_VM_BENCH_RUNNER_H_

#include <cstddef>
#include <cstdint>

namespace gaby_vm_bench {

int RunBenchmark(const char* workload_name,
                 const char* generator_tag,
                 const uint32_t* code,
                 std::size_t static_word_count,
                 std::uint64_t dynamic_insns_per_iter,
                 int argc,
                 char* argv[]);

}  // namespace gaby_vm_bench

#endif  // GABY_VM_BENCH_RUNNER_H_
