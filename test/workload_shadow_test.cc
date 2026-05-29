// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// workload_shadow_test: runs the ShadowRunner oracle over every committed
// benchmark workload and asserts zero cache-vs-decoder divergence.
//
// This is V1's hard correctness acceptance (design doc 4.5 acceptance #3): if
// the predecode cache mis-executes any instruction in a real workload, the
// lockstep oracle catches it. It consumes only the committed workload data
// headers under bench/workloads/ — it does not build or run the benchmark
// harness itself.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/shadow_runner.h"
#include "gaby_vm/simulator.h"
#include "workloads/mixed_workload_data.h"
#include "workloads/smoke_workload_data.h"

namespace {

int g_passed = 0;
int g_total = 0;

void check(bool ok, const char* label) {
  g_total += 1;
  if (ok) {
    g_passed += 1;
  } else {
    std::fprintf(stderr, "[FAIL] %s\n", label);
  }
}

// Identity branch hook used by the identity-hook workload pass. Returns the
// architectural target unchanged so the run is observationally identical to
// one with no hook installed (branch-hook-api specs/aarch64-simulator:
// "Identity hook is observationally invisible").
uintptr_t identity_hook(uintptr_t target_pc,
                        void* /*user_data*/,
                        gaby_vm::Simulator& /*sim*/) {
  return target_pc;
}

// Register `code` in a fresh cache, then run the cache and decoder tracks in
// lockstep over it. Returns true iff the run completes with no divergence.
// `install_identity_hook` mirrors the same identity branch hook onto both
// inner Simulators (branch-hook-api): the hook must not introduce any
// divergence relative to the no-hook baseline.
bool shadow_workload(const char* name,
                     const uint32_t* code,
                     size_t word_count,
                     bool install_identity_hook = false) {
  gaby_vm::PredecodeCache cache;
  const gaby_vm::PredecodeCache::RegistrationStatus status =
      cache.RegisterCodeRange(code, word_count * sizeof(uint32_t));
  if (status != gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    const gaby_vm::PredecodeCache::RegistrationError* detail =
        cache.GetLastRegistrationError();
    std::fprintf(stderr,
                 "  %s: RegisterCodeRange failed: %s\n",
                 name,
                 (detail != nullptr) ? detail->reason : "(no detail)");
    return false;
  }

  // A generous heap-resident guest stack, shared by both tracks.
  std::vector<uint8_t> stack(1024 * 1024);
  gaby_vm::testing::ShadowRunner shadow(&cache, stack.data(), stack.size());

  if (install_identity_hook) {
    shadow.SetBranchHook(identity_hook, nullptr);
  }

  int divergences = 0;
  gaby_vm::testing::DivergenceReport first{};
  shadow.SetDivergenceHandler(
      [&divergences, &first](const gaby_vm::testing::DivergenceReport& report) {
        if (divergences == 0) {
          first = report;
        }
        ++divergences;
      });

  shadow.RunFrom(reinterpret_cast<uintptr_t>(code));

  if (divergences != 0) {
    if (first.kind == gaby_vm::testing::DivergenceReport::Kind::Register) {
      std::fprintf(stderr,
                   "  %s: divergence at pc 0x%llx, register %s "
                   "(cache 0x%llx%016llx vs decoder 0x%llx%016llx)\n",
                   name,
                   static_cast<unsigned long long>(first.pc),
                   first.reg_name,
                   static_cast<unsigned long long>(first.fast_hi),
                   static_cast<unsigned long long>(first.fast_lo),
                   static_cast<unsigned long long>(first.ref_hi),
                   static_cast<unsigned long long>(first.ref_lo));
    } else {
      std::fprintf(stderr,
                   "  %s: divergence at pc 0x%llx, memory write to 0x%llx\n",
                   name,
                   static_cast<unsigned long long>(first.pc),
                   static_cast<unsigned long long>(first.mem_addr));
    }
    return false;
  }

  std::printf("  %s: %zu static words, zero divergence\n", name, word_count);
  return true;
}

}  // namespace

int main() {
  check(shadow_workload("smoke",
                        gaby_vm_bench::kSmokeWorkloadInstructions,
                        gaby_vm_bench::kSmokeWorkloadStaticWordCount),
        "smoke workload runs diverge-free under ShadowRunner");

  check(shadow_workload("mixed",
                        gaby_vm_bench::kMixedWorkloadInstructions,
                        gaby_vm_bench::kMixedWorkloadStaticWordCount),
        "mixed workload runs diverge-free under ShadowRunner");

  // Second pass: same workloads, with an identity branch hook installed on
  // both tracks. The hook returns BranchAction{ target_pc, false } unchanged,
  // so the run MUST be observationally indistinguishable from the no-hook
  // pass above. Pins down the spec's "Identity hook is observationally
  // invisible" scenario.
  check(shadow_workload("smoke (identity hook)",
                        gaby_vm_bench::kSmokeWorkloadInstructions,
                        gaby_vm_bench::kSmokeWorkloadStaticWordCount,
                        /*install_identity_hook=*/true),
        "smoke workload diverge-free with identity branch hook installed");

  check(shadow_workload("mixed (identity hook)",
                        gaby_vm_bench::kMixedWorkloadInstructions,
                        gaby_vm_bench::kMixedWorkloadStaticWordCount,
                        /*install_identity_hook=*/true),
        "mixed workload diverge-free with identity branch hook installed");

  std::printf("workload_shadow: %d/%d workloads diverge-free\n",
              g_passed,
              g_total);
  return (g_passed == g_total) ? 0 : 1;
}
