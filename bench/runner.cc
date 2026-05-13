#include "runner.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "cpu-features.h"

#include "aarch64/decoder-aarch64.h"
#include "aarch64/simulator-aarch64.h"

namespace gaby_vm_bench {

namespace {

constexpr double kDefaultRunSeconds = 5.0;

struct Args {
  double seconds = kDefaultRunSeconds;
};

bool ParseArgs(int argc, char* argv[], Args* out) {
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--seconds") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for --seconds\n");
        return false;
      }
      const char* v = argv[++i];
      char* endp = nullptr;
      const double s = std::strtod(v, &endp);
      if (endp == v || s <= 0.0) {
        std::fprintf(stderr, "invalid --seconds value: %s\n", v);
        return false;
      }
      out->seconds = s;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", a);
      return false;
    }
  }
  return true;
}

}  // namespace

int RunBenchmark(const char* workload_name,
                 const char* generator_tag,
                 const uint32_t* code,
                 std::size_t static_word_count,
                 std::uint64_t dynamic_insns_per_iter,
                 int argc,
                 char* argv[]) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    return 2;
  }

  // Copy the const instruction stream into a simulator-visible buffer.
  // The simulator only reads, but a heap-resident copy keeps lifetime
  // and alignment behavior identical across workloads regardless of
  // where the source header chose to place its literal.
  std::vector<std::uint32_t> buffer(code, code + static_word_count);
  const auto* start =
      reinterpret_cast<const vixl::aarch64::Instruction*>(buffer.data());

  vixl::aarch64::Decoder decoder;
  vixl::aarch64::Simulator sim(&decoder);
  sim.SetCPUFeatures(vixl::CPUFeatures::All());

  // Warm-up — discarded. The Simulator's ResetRegisters() already wrote
  // kEndOfSimAddress to LR at construction time; we re-write defensively
  // so the warm-up uses the same convention as the timed iterations.
  sim.WriteLr(vixl::aarch64::Simulator::kEndOfSimAddress);
  sim.RunFrom(start);

  const auto target_dur = std::chrono::duration<double>(args.seconds);
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t iterations = 0;
  do {
    sim.WriteLr(vixl::aarch64::Simulator::kEndOfSimAddress);
    sim.RunFrom(start);
    ++iterations;
  } while (std::chrono::steady_clock::now() - t0 < target_dur);
  const auto t1 = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(t1 - t0).count();

  const std::uint64_t total_insns = iterations * dynamic_insns_per_iter;
  const double iters_per_sec = static_cast<double>(iterations) / elapsed;
  const double throughput = static_cast<double>(total_insns) / elapsed;
  const double ns_per_insn = (elapsed * 1e9) / static_cast<double>(total_insns);

  std::printf("workload: %s\n", workload_name);
  std::printf("workload_generator_tag: %s\n", generator_tag);
  std::printf("static_words_in_buffer: %zu\n", static_word_count);
  std::printf("dynamic_instructions_per_iteration: %" PRIu64 "\n",
              dynamic_insns_per_iter);
  std::printf("iterations: %" PRIu64 "\n", iterations);
  std::printf("total_dynamic_instructions: %" PRIu64 "\n", total_insns);
  std::printf("elapsed_seconds: %.6f\n", elapsed);
  std::printf("iterations_per_second: %.6f\n", iters_per_sec);
  std::printf("throughput_insn_per_sec: %.6f\n", throughput);
  std::printf("ns_per_instruction: %.6f\n", ns_per_insn);
  return 0;
}

}  // namespace gaby_vm_bench
