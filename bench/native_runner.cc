#include "native_runner.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>  // sys_icache_invalidate
#include <pthread.h>                 // pthread_jit_write_protect_np
#endif

// The native baseline executes guest AArch64 code directly on the host, so the
// host must be arm64. bench/CMakeLists.txt already gates the target on the host
// processor; this is the in-source backstop for an out-of-tree compile.
#if !defined(__aarch64__) && !defined(_M_ARM64)
#error "native baseline requires an arm64 host (guest ISA == host ISA)"
#endif

// See bench/CMakeLists.txt; injected to match the bench_runner output's
// build_type key. The fallback covers an out-of-tree compile.
#ifndef GABY_VM_BENCH_BUILD_TYPE
#define GABY_VM_BENCH_BUILD_TYPE "Unknown"
#endif

namespace gaby_vm_bench {

namespace {

constexpr double kDefaultRunSeconds = 5.0;
constexpr double kMinSeconds = 0.001;

// Brief discarded warm-up: enough calls to fault-check the executable mapping
// and prime caches + branch predictor before the timed region. The committed
// workloads reconverge their branches every pass, so a small fixed count warms
// the steady state regardless of workload size. The warm-up is also timed and
// reused to calibrate the batch size below.
constexpr int kWarmupIterations = 2000;

// The timed loop runs the workload in batches and only reads the clock once
// per batch, so steady_clock::now() (~tens of ns) stays negligible even for the
// smoke workload, where a single pass is a couple of nanoseconds and a
// per-iteration clock read would otherwise dominate the measurement. The batch
// size is calibrated from the warm-up so each batch spans roughly this long;
// it also bounds how far past --seconds the loop can overshoot (one batch).
constexpr double kBatchSeconds = 0.001;

constexpr const char* kBuildType =
    *GABY_VM_BENCH_BUILD_TYPE ? GABY_VM_BENCH_BUILD_TYPE : "Unknown";

using WorkloadFn = void (*)();

struct Args {
  double seconds = kDefaultRunSeconds;
};

enum class ParseResult { kRun, kHelpAndExit, kErrorAndExit };

void PrintUsage(const char* program_name,
                const char* workload_description,
                std::FILE* out) {
  std::fprintf(out,
               "Usage: %s [--seconds <float>] [--help|-h]\n"
               "\n"
               "%s\n"
               "\n"
               "Executes the same committed workload bytes natively on the "
               "host\n"
               "arm64 CPU (mode: native), as a reference for the decoder- and "
               "cache-\n"
               "mode numbers from bench_smoke / bench_baseline.\n"
               "\n"
               "Options:\n"
               "  --seconds <float>   Timed-loop target duration in seconds.\n"
               "                      Default: %.1f. Minimum: %g.\n"
               "  --help, -h          Show this message and exit.\n",
               program_name,
               workload_description,
               kDefaultRunSeconds,
               kMinSeconds);
}

ParseResult ParseArgs(int argc, char* argv[], Args* out) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      return ParseResult::kHelpAndExit;
    }
  }

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--seconds") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for --seconds\n");
        return ParseResult::kErrorAndExit;
      }
      const char* v = argv[++i];
      char* endp = nullptr;
      const double s = std::strtod(v, &endp);
      if (endp == v || s <= 0.0) {
        std::fprintf(stderr, "invalid --seconds value: %s\n", v);
        return ParseResult::kErrorAndExit;
      }
      if (s < kMinSeconds) {
        std::fprintf(stderr,
                     "--seconds value %g is below minimum %g\n",
                     s,
                     kMinSeconds);
        return ParseResult::kErrorAndExit;
      }
      out->seconds = s;
    } else {
      std::fprintf(stderr,
                   "unknown argument: %s (use --help for the supported flag "
                   "list)\n",
                   a);
      return ParseResult::kErrorAndExit;
    }
  }
  return ParseResult::kRun;
}

// Copies `code` into executable memory and returns a callable pointer, or
// nullptr on failure (with a message already printed to stderr). On Apple
// platforms the W^X rule requires MAP_JIT plus the pthread_jit_write_protect_np
// toggle; elsewhere on arm64, mmap(RW) + mprotect(RX) + a cache flush suffices.
WorkloadFn MapExecutable(const std::uint32_t* code, std::size_t word_count) {
  const std::size_t bytes = word_count * sizeof(std::uint32_t);

#if defined(__APPLE__)
  void* mem = mmap(nullptr,
                   bytes,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT,
                   -1,
                   0);
  if (mem == MAP_FAILED) {
    std::perror("mmap(MAP_JIT)");
    return nullptr;
  }
  // Disable write-protect (region writable), copy, re-enable (region
  // executable), then sync the instruction cache for the freshly written code.
  pthread_jit_write_protect_np(0);
  std::memcpy(mem, code, bytes);
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(mem, bytes);
#else
  void* mem = mmap(nullptr,
                   bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON,
                   -1,
                   0);
  if (mem == MAP_FAILED) {
    std::perror("mmap");
    return nullptr;
  }
  std::memcpy(mem, code, bytes);
  if (mprotect(mem, bytes, PROT_READ | PROT_EXEC) != 0) {
    std::perror("mprotect");
    return nullptr;
  }
  __builtin___clear_cache(reinterpret_cast<char*>(mem),
                          reinterpret_cast<char*>(mem) + bytes);
#endif

  // Casting an object pointer to a function pointer is conditionally-supported
  // (and -Wpedantic-flagged); copy the bits instead to keep the strict
  // project warning policy clean.
  WorkloadFn fn = nullptr;
  std::memcpy(&fn, &mem, sizeof(fn));
  return fn;
}

}  // namespace

int RunNativeBaseline(const char* workload_name,
                      const char* workload_description,
                      const char* generator_tag,
                      const std::uint32_t* code,
                      std::size_t static_word_count,
                      std::uint64_t dynamic_insns_per_iter,
                      int argc,
                      char* argv[]) {
  Args args;
  switch (ParseArgs(argc, argv, &args)) {
    case ParseResult::kHelpAndExit:
      PrintUsage(argv[0] != nullptr ? argv[0] : workload_name,
                 workload_description,
                 stdout);
      return 0;
    case ParseResult::kErrorAndExit:
      return 2;
    case ParseResult::kRun:
      break;
  }

  WorkloadFn run = MapExecutable(code, static_word_count);
  if (run == nullptr) {
    std::fprintf(stderr, "native baseline: failed to map executable memory\n");
    return 2;
  }

  // Discarded warm-up — also the first real execution (so a bad mapping faults
  // here, not in the timed region) and the calibration sample for the batch
  // size: time it to estimate per-pass cost.
  const auto w0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kWarmupIterations; ++i) {
    run();
  }
  const auto w1 = std::chrono::steady_clock::now();
  const double per_iter_seconds =
      std::max(std::chrono::duration<double>(w1 - w0).count() /
                   static_cast<double>(kWarmupIterations),
               1e-9);
  // Batch enough passes to span ~kBatchSeconds between clock reads, clamped so
  // a too-fast or too-slow estimate can't produce a degenerate batch.
  std::uint64_t batch =
      static_cast<std::uint64_t>(kBatchSeconds / per_iter_seconds);
  if (batch < 1) {
    batch = 1;
  }
  if (batch > 100000000ULL) {
    batch = 100000000ULL;
  }

  const auto target_dur = std::chrono::duration<double>(args.seconds);
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t iterations = 0;
  do {
    for (std::uint64_t b = 0; b < batch; ++b) {
      run();
    }
    iterations += batch;
  } while (std::chrono::steady_clock::now() - t0 < target_dur);
  const auto t1 = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(t1 - t0).count();

  const std::uint64_t total_insns = iterations * dynamic_insns_per_iter;
  const double iters_per_sec = static_cast<double>(iterations) / elapsed;
  const double throughput = static_cast<double>(total_insns) / elapsed;
  const double ns_per_insn = (elapsed * 1e9) / static_cast<double>(total_insns);

  // dynamic_instructions_per_iteration is the simulator's measured count. The
  // native run may take a marginally different dynamic path (initial register
  // values differ -> a few early branch outcomes differ), but the committed
  // workloads reconverge their branches, so the count is effectively
  // path-independent. iterations_per_second (full workload passes per second)
  // is the exact, directly-comparable metric against decoder/cache mode;
  // throughput/ns_per_instruction inherit the count as a close approximation.
  std::printf("workload: %s\n", workload_name);
  std::printf("build_type: %s\n", kBuildType);
  std::printf("mode: native\n");
  std::printf("branch_hook: n/a\n");
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
