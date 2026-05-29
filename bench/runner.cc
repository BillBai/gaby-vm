#include "runner.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "cpu-features.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

#include "aarch64/decoder-aarch64.h"
#include "aarch64/simulator-aarch64.h"

// GABY_VM_BENCH_BUILD_TYPE is normally injected by bench/CMakeLists.txt as a
// string literal matching the active CMake configuration (e.g. "Release",
// "Debug"). The fallback covers builds that compile runner.cc outside of the
// project's CMake (rare, but cheap to defend against). The runtime ternary in
// kBuildType below additionally guards the multi-config edge case where
// $<CONFIG> resolves to an empty string (no active config selected). See
// openspec/changes/bench-runner-enhancements/design.md decision 2.
#ifndef GABY_VM_BENCH_BUILD_TYPE
#define GABY_VM_BENCH_BUILD_TYPE "Unknown"
#endif

namespace gaby_vm_bench {

namespace {

constexpr double kDefaultRunSeconds = 5.0;
// Minimum --seconds value. Values strictly below this are rejected with an
// error before any simulator work runs. See design.md decision 4 for why
// 1 ms was chosen.
constexpr double kMinSeconds = 0.001;

constexpr const char* kBuildType =
    *GABY_VM_BENCH_BUILD_TYPE ? GABY_VM_BENCH_BUILD_TYPE : "Unknown";

// Which execution mode drives the workload. `kDecoder` keeps the historic
// behaviour — `vixl::aarch64::Simulator` driven directly by the imported
// decoder + visitor flow — and stays the default. `kCache` selects the
// gaby_vm cache-track `Simulator` over a `PredecodeCache`. The terminology
// is documented in docs/conventions.md ("decoder mode" / "cache mode") and
// is the same vocabulary used by the `--mode` CLI flag and the `mode:`
// output key.
enum class Mode { kDecoder, kCache };

constexpr const char* ModeName(Mode m) {
  switch (m) {
    case Mode::kDecoder:
      return "decoder";
    case Mode::kCache:
      return "cache";
  }
  return "unknown";
}

// Branch-hook variant (cache mode only; decoder mode bypasses gaby_vm and
// therefore has no SetBranchHook entry point). Three settings:
//   kNone      — no SetBranchHook call. The bench's historical behaviour.
//   kNull      — SetBranchHook(nullptr, nullptr). Observationally identical
//                to kNone but exercises the null-check fast path in the
//                leaf hook helper. Catches accidental regressions in the
//                null-hook path (branch-hook-api tasks.md task 8.2).
//   kIdentity  — install a tiny `return target_pc;` hook. Each taken branch
//                costs one indirect call. Useful as an informational
//                measurement of the hot-path overhead ceiling
//                (branch-hook-api tasks.md task 8.3).
enum class HookVariant { kNone, kNull, kIdentity };

constexpr const char* HookVariantName(HookVariant v) {
  switch (v) {
    case HookVariant::kNone:
      return "none";
    case HookVariant::kNull:
      return "null";
    case HookVariant::kIdentity:
      return "identity";
  }
  return "unknown";
}

uintptr_t IdentityBranchHook(uintptr_t target_pc,
                             void* /*user_data*/,
                             gaby_vm::Simulator& /*sim*/) {
  return target_pc;
}

struct Args {
  double seconds = kDefaultRunSeconds;
  Mode mode = Mode::kDecoder;
  HookVariant hook = HookVariant::kNone;
};

enum class ParseResult { kRun, kHelpAndExit, kErrorAndExit };

void PrintUsage(const char* program_name,
                const char* workload_description,
                std::FILE* out) {
  std::fprintf(out,
               "Usage: %s [--mode {decoder|cache}] "
               "[--hook {none|null|identity}] "
               "[--seconds <float>] [--help|-h]\n"
               "\n"
               "%s\n"
               "\n"
               "Options:\n"
               "  --mode <mode>       Execution mode to drive the workload.\n"
               "                      'decoder' (default) drives the imported "
               "VIXL\n"
               "                      simulator directly. 'cache' drives the "
               "gaby_vm\n"
               "                      cache-track Simulator over a "
               "PredecodeCache; the\n"
               "                      buffer is registered before the warm-up "
               "call.\n"
               "  --hook <variant>    Branch-hook variant (cache mode only).\n"
               "                      'none' (default): no SetBranchHook "
               "call.\n"
               "                      'null': SetBranchHook(nullptr, nullptr)\n"
               "                              — exercises the null-check\n"
               "                              fast path.\n"
               "                      'identity': install an identity hook —\n"
               "                              exercises the indirect-call\n"
               "                              path on every taken branch.\n"
               "  --seconds <float>   Timed-loop target duration in seconds.\n"
               "                      Default: %.1f. Minimum: %g.\n"
               "  --help, -h          Show this message and exit.\n",
               program_name,
               workload_description,
               kDefaultRunSeconds,
               kMinSeconds);
}

ParseResult ParseArgs(int argc, char* argv[], Args* out) {
  // --help / -h is recognised at any position and short-circuits all other
  // parsing (design.md decision 5), including the unknown-argument path.
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
    } else if (std::strcmp(a, "--mode") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for --mode\n");
        return ParseResult::kErrorAndExit;
      }
      const char* v = argv[++i];
      if (std::strcmp(v, "decoder") == 0) {
        out->mode = Mode::kDecoder;
      } else if (std::strcmp(v, "cache") == 0) {
        out->mode = Mode::kCache;
      } else {
        std::fprintf(stderr,
                     "invalid --mode value: %s (expected 'decoder' or "
                     "'cache')\n",
                     v);
        return ParseResult::kErrorAndExit;
      }
    } else if (std::strcmp(a, "--hook") == 0) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for --hook\n");
        return ParseResult::kErrorAndExit;
      }
      const char* v = argv[++i];
      if (std::strcmp(v, "none") == 0) {
        out->hook = HookVariant::kNone;
      } else if (std::strcmp(v, "null") == 0) {
        out->hook = HookVariant::kNull;
      } else if (std::strcmp(v, "identity") == 0) {
        out->hook = HookVariant::kIdentity;
      } else {
        std::fprintf(stderr,
                     "invalid --hook value: %s (expected 'none', 'null' or "
                     "'identity')\n",
                     v);
        return ParseResult::kErrorAndExit;
      }
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

struct RunResult {
  double elapsed_seconds;
  std::uint64_t iterations;
};

// Decoder mode — drives the imported `vixl::aarch64::Simulator` directly.
// This is the historic harness path; preserved here unchanged so cache-mode
// and decoder-mode runs report against an identical baseline.
RunResult RunDecoderMode(const std::uint32_t* code,
                         std::size_t static_word_count,
                         double target_seconds) {
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

  const auto target_dur = std::chrono::duration<double>(target_seconds);
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t iterations = 0;
  do {
    sim.WriteLr(vixl::aarch64::Simulator::kEndOfSimAddress);
    sim.RunFrom(start);
    ++iterations;
  } while (std::chrono::steady_clock::now() - t0 < target_dur);
  const auto t1 = std::chrono::steady_clock::now();
  return {std::chrono::duration<double>(t1 - t0).count(), iterations};
}

// Cache mode — drives the public `gaby_vm::Simulator` over a
// `PredecodeCache`. The workload buffer is registered as a code range
// *before* the warm-up call (design D3), so the timed region measures
// steady-state cache execution only — the one-time predecode pass is
// already paid by the time the steady-state loop starts.
//
// The warm-up + timed-loop shape mirrors `RunDecoderMode` exactly: one
// discarded warm-up `RunFrom` followed by a steady-state loop that re-arms
// the link-register sentinel before each iteration. `LR = 0` is the
// end-of-simulation sentinel for both modes (imported VIXL's
// `kEndOfSimAddress` is `NULL`).
RunResult RunCacheMode(const std::uint32_t* code,
                       std::size_t static_word_count,
                       double target_seconds,
                       HookVariant hook) {
  std::vector<std::uint32_t> buffer(code, code + static_word_count);
  const std::size_t buffer_bytes = buffer.size() * sizeof(std::uint32_t);
  const std::uintptr_t entry_pc =
      reinterpret_cast<std::uintptr_t>(buffer.data());

  gaby_vm::PredecodeCache cache;
  const gaby_vm::PredecodeCache::RegistrationStatus status =
      cache.RegisterCodeRange(buffer.data(), buffer_bytes);
  if (status != gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    const gaby_vm::PredecodeCache::RegistrationError* err =
        cache.GetLastRegistrationError();
    std::fprintf(stderr,
                 "cache mode: RegisterCodeRange failed: %s\n",
                 (err != nullptr && err->reason != nullptr) ? err->reason
                                                            : "(no detail)");
    std::exit(2);
  }

  // The simulator only needs enough stack for the workload's own SP
  // adjustments; the committed workloads stay well within the floor.
  std::vector<std::uint8_t> stack(gaby_vm::Simulator::kMinStackSize);
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  // Branch-hook variant. Install before the warm-up so the warm-up exercises
  // the same hot path the timed iterations will. See HookVariant docs above.
  switch (hook) {
    case HookVariant::kNone:
      break;
    case HookVariant::kNull:
      sim.SetBranchHook(nullptr, nullptr);
      break;
    case HookVariant::kIdentity:
      sim.SetBranchHook(IdentityBranchHook, nullptr);
      break;
  }

  // Warm-up — discarded. The Simulator's construction already seated LR at
  // the end-of-simulation sentinel; we re-write defensively so the warm-up
  // uses the same convention as the timed iterations.
  sim.Write(gaby_vm::GpRegister::LR, 0);
  sim.RunFrom(entry_pc);

  const auto target_dur = std::chrono::duration<double>(target_seconds);
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t iterations = 0;
  do {
    sim.Write(gaby_vm::GpRegister::LR, 0);
    sim.RunFrom(entry_pc);
    ++iterations;
  } while (std::chrono::steady_clock::now() - t0 < target_dur);
  const auto t1 = std::chrono::steady_clock::now();
  return {std::chrono::duration<double>(t1 - t0).count(), iterations};
}

}  // namespace

int RunBenchmark(const char* workload_name,
                 const char* workload_description,
                 const char* generator_tag,
                 const uint32_t* code,
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

  const RunResult result =
      (args.mode == Mode::kCache)
          ? RunCacheMode(code, static_word_count, args.seconds, args.hook)
          : RunDecoderMode(code, static_word_count, args.seconds);

  const double elapsed = result.elapsed_seconds;
  const std::uint64_t iterations = result.iterations;
  const std::uint64_t total_insns = iterations * dynamic_insns_per_iter;
  const double iters_per_sec = static_cast<double>(iterations) / elapsed;
  const double throughput = static_cast<double>(total_insns) / elapsed;
  const double ns_per_insn = (elapsed * 1e9) / static_cast<double>(total_insns);

  std::printf("workload: %s\n", workload_name);
  std::printf("build_type: %s\n", kBuildType);
  std::printf("mode: %s\n", ModeName(args.mode));
  std::printf("branch_hook: %s\n", HookVariantName(args.hook));
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
