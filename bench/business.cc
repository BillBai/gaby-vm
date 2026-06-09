// bench_business — business-logic microkernel benchmark driver.
//
// Drives a small suite of self-contained, scalar (no-NEON, no-syscall,
// no-external-call) microkernels that model the iOS hot-fix business-logic
// scenario: a serialized-record parser (`parse`), an integer-dense digest
// (`hash`), a struct-array transform (`struct`), and a branch-dense scanner
// (`fsm`). Each kernel is reported separately so the slowdown-vs-native spread
// is visible per business-logic shape — which is the whole point: it tells us
// where the interpreter is weak (branchy dispatch on parse/fsm) versus where it
// already approaches the scalar ceiling (hash).
//
// Two roles:
//   * default — measure each selected kernel via the shared RunBenchmark timing
//     loop (same `key: value` output as bench_smoke / bench_baseline; the
//     `--mode`/`--seconds`/`--hook` flags pass straight through).
//   * --verify — single-step each kernel on both the cache and decoder tracks,
//     count the dynamic instructions, and cross-check the x0 result across both
//     tracks and the committed expected value. This is the benchmark's own
//     correctness gate and the source of the committed dynamic-count / expected
//     constants (the gen script seeds them to 0; this pass prints the truth).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"
#include "runner.h"
#include "workloads/business/applogic_workload_data.h"
#include "workloads/business/fsm_workload_data.h"
#include "workloads/business/hash_workload_data.h"
#include "workloads/business/oracle_data.h"
#include "workloads/business/parse_workload_data.h"
#include "workloads/business/struct_workload_data.h"

namespace {

struct Kernel {
  const char* name;
  const char* description;
  const char* tag;
  const std::uint32_t* code;
  std::size_t static_words;
  std::uint64_t dynamic_per_iter;
  std::uint64_t expected_result;
};

constexpr Kernel kKernels[] = {
    {"parse",
     "Serialized-record parser + validator (varint/TLV wire decode). "
     "Branch-dense, data-dependent dispatch.",
     gaby_vm_bench::kParseWorkloadGeneratorTag,
     gaby_vm_bench::kParseWorkloadInstructions,
     gaby_vm_bench::kParseWorkloadStaticWordCount,
     gaby_vm_bench::kParseWorkloadDynamicInstructionsPerIteration,
     gaby_vm_bench::kParseWorkloadExpectedResult},
    {"hash",
     "Integer-dense digest (FNV-1a + avalanche mix). ALU-bound, "
     "branch-light — the interpreter's best case.",
     gaby_vm_bench::kHashWorkloadGeneratorTag,
     gaby_vm_bench::kHashWorkloadInstructions,
     gaby_vm_bench::kHashWorkloadStaticWordCount,
     gaby_vm_bench::kHashWorkloadDynamicInstructionsPerIteration,
     gaby_vm_bench::kHashWorkloadExpectedResult},
    {"struct",
     "Struct-array transform (offset load/store + branchy derivation). "
     "Exercises the scalar load/store leaf path.",
     gaby_vm_bench::kStructWorkloadGeneratorTag,
     gaby_vm_bench::kStructWorkloadInstructions,
     gaby_vm_bench::kStructWorkloadStaticWordCount,
     gaby_vm_bench::kStructWorkloadDynamicInstructionsPerIteration,
     gaby_vm_bench::kStructWorkloadExpectedResult},
    {"fsm",
     "Branch-dense state-machine scanner (tokenizer/validator). "
     "Unpredictable per-byte dispatch — the interpreter's worst case.",
     gaby_vm_bench::kFsmWorkloadGeneratorTag,
     gaby_vm_bench::kFsmWorkloadInstructions,
     gaby_vm_bench::kFsmWorkloadStaticWordCount,
     gaby_vm_bench::kFsmWorkloadDynamicInstructionsPerIteration,
     gaby_vm_bench::kFsmWorkloadExpectedResult},
    {"applogic",
     "Mixed app-logic kernel: integer business logic + scalar-double-FP layout "
     "geometry + a little NEON. The only FP/NEON kernel — models the real iOS "
     "instruction mix (CGFloat geometry is double FP) the scalar four omit.",
     gaby_vm_bench::kApplogicWorkloadGeneratorTag,
     gaby_vm_bench::kApplogicWorkloadInstructions,
     gaby_vm_bench::kApplogicWorkloadStaticWordCount,
     gaby_vm_bench::kApplogicWorkloadDynamicInstructionsPerIteration,
     gaby_vm_bench::kApplogicWorkloadExpectedResult},
};

constexpr std::size_t kKernelCount = sizeof(kKernels) / sizeof(kKernels[0]);

const Kernel* FindKernel(const char* name) {
  for (const Kernel& k : kKernels) {
    if (std::strcmp(k.name, name) == 0) {
      return &k;
    }
  }
  return nullptr;
}

// Single-step one track to termination, counting executed instructions and
// returning the final x0. `cache_track` selects StepOnce (cache) vs
// DebugStepOnce (decoder). Both run the same bytes from a fresh architectural
// state, so a divergent x0 or count means the cache track and the reference
// decoder disagree — a real bug, not a benchmark artifact.
struct StepResult {
  std::uint64_t count;
  std::uint64_t x0;
};

StepResult CountTrack(const std::uint32_t* code,
                      std::size_t words,
                      bool cache_track) {
  std::vector<std::uint32_t> buffer(code, code + words);
  const std::size_t bytes = buffer.size() * sizeof(std::uint32_t);
  const std::uintptr_t entry = reinterpret_cast<std::uintptr_t>(buffer.data());

  gaby_vm::PredecodeCache cache;
  if (cache.RegisterCodeRange(buffer.data(), bytes) !=
      gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    std::fprintf(stderr, "verify: RegisterCodeRange failed\n");
    std::exit(2);
  }

  std::vector<std::uint8_t> stack(gaby_vm::Simulator::kMinStackSize);
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
  sim.Write(gaby_vm::GpRegister::LR, 0);

  std::uint64_t count = 0;
  const bool first =
      cache_track ? sim.StepOnce(entry) : sim.DebugStepOnce(entry);
  if (first) {
    count = 1;
    while (cache_track ? sim.StepOnce() : sim.DebugStepOnce()) {
      ++count;
    }
  }
  return {count, sim.Read(gaby_vm::GpRegister::X0)};
}

// Verify one kernel: count + oracle on both tracks. Returns true on agreement.
bool VerifyKernel(const Kernel& k) {
  const StepResult cache = CountTrack(k.code, k.static_words, true);
  const StepResult decoder = CountTrack(k.code, k.static_words, false);

  const bool count_ok = cache.count == decoder.count;
  const bool x0_ok = cache.x0 == decoder.x0;
  const bool expected_ok =
      k.expected_result == 0 || cache.x0 == k.expected_result;
  const bool ok = count_ok && x0_ok && expected_ok;

  std::printf("kernel: %s\n", k.name);
  std::printf("  dynamic_instructions: cache=%llu decoder=%llu%s\n",
              static_cast<unsigned long long>(cache.count),
              static_cast<unsigned long long>(decoder.count),
              count_ok ? "" : "  <-- MISMATCH");
  std::printf("  x0: cache=0x%016llx decoder=0x%016llx%s\n",
              static_cast<unsigned long long>(cache.x0),
              static_cast<unsigned long long>(decoder.x0),
              x0_ok ? "" : "  <-- MISMATCH");
  if (k.expected_result != 0 && !expected_ok) {
    std::printf("  committed_expected=0x%016llx  <-- MISMATCH\n",
                static_cast<unsigned long long>(k.expected_result));
  }
  // Copy-pasteable constants for the committed header.
  std::printf(
      "  >> patch %s_workload_data.h: Dynamic=%llu Expected=0x%016llxull\n",
      k.name,
      static_cast<unsigned long long>(cache.count),
      static_cast<unsigned long long>(cache.x0));
  return ok;
}

void PrintUsage(const char* prog) {
  std::printf(
      "Usage: %s [--kernel {all|parse|hash|struct|fsm|applogic}] [--verify]\n"
      "          [--mode {decoder|cache}] [--hook {none|null|identity}]\n"
      "          [--seconds <float>] [--help|-h]\n"
      "\n"
      "Business-logic microkernel suite. Without --kernel, runs all five.\n"
      "--verify single-steps each kernel on both tracks, counts dynamic\n"
      "instructions, and cross-checks the x0 result (cache vs decoder vs\n"
      "committed expected). All other flags pass through to the shared\n"
      "timing loop; see bench_smoke --help for their meaning.\n",
      prog);
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* selected = "all";
  bool verify = false;

  // Pull out --kernel and --verify; forward everything else to RunBenchmark /
  // the verify path. --help is handled here so it can describe the extra flags.
  std::vector<char*> forward;
  forward.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(argv[i], "--kernel") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for --kernel\n");
        return 2;
      }
      selected = argv[++i];
      continue;
    }
    if (std::strcmp(argv[i], "--verify") == 0) {
      verify = true;
      continue;
    }
    forward.push_back(argv[i]);
  }

  const bool all = std::strcmp(selected, "all") == 0;
  if (!all && FindKernel(selected) == nullptr) {
    std::fprintf(stderr,
                 "unknown kernel: %s (expected "
                 "all|parse|hash|struct|fsm|applogic)\n",
                 selected);
    return 2;
  }

  if (verify) {
    bool ok = true;
    for (const Kernel& k : kKernels) {
      if (all || std::strcmp(k.name, selected) == 0) {
        ok &= VerifyKernel(k);
      }
    }
    if (!ok) {
      std::fprintf(stderr, "verify: FAILED — track mismatch (see above)\n");
      return 1;
    }
    std::printf("verify: OK (all selected kernels agree)\n");
    return 0;
  }

  int rc = 0;
  for (std::size_t idx = 0; idx < kKernelCount; ++idx) {
    const Kernel& k = kKernels[idx];
    if (!all && std::strcmp(k.name, selected) != 0) {
      continue;
    }
    if (idx != 0 && (all)) {
      std::printf("\n");
    }
    const int r = gaby_vm_bench::RunBenchmark(k.name,
                                              k.description,
                                              k.tag,
                                              k.code,
                                              k.static_words,
                                              k.dynamic_per_iter,
                                              static_cast<int>(forward.size()),
                                              forward.data());
    if (r != 0) {
      rc = r;
    }
  }
  return rc;
}
