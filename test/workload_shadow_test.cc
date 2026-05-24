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

// Register `code` in a fresh cache, then run the cache and decoder tracks in
// lockstep over it. Returns true iff the run completes with no divergence.
bool shadow_workload(const char* name,
                     const uint32_t* code,
                     size_t word_count) {
  gaby_vm::PredecodeCache cache;
  const gaby_vm::RegisterStatus status =
      cache.RegisterCodeRange(code, word_count * sizeof(uint32_t));
  if (status != gaby_vm::RegisterStatus::Ok) {
    const gaby_vm::ErrorDetail* detail = cache.GetLastErrorDetail();
    std::fprintf(stderr,
                 "  %s: RegisterCodeRange failed: %s\n",
                 name,
                 (detail != nullptr) ? detail->reason : "(no detail)");
    return false;
  }

  // A generous heap-resident guest stack, shared by both tracks.
  std::vector<uint8_t> stack(1024 * 1024);
  gaby_vm::testing::ShadowRunner shadow(&cache, stack.data(), stack.size());

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

  std::printf("workload_shadow: %d/%d workloads diverge-free\n",
              g_passed,
              g_total);
  return (g_passed == g_total) ? 0 : 1;
}
