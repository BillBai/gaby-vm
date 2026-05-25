// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// shadow_runner_test: self-test for the gaby_vm::testing::ShadowRunner oracle.
//
// ShadowRunner is the V1 correctness oracle — it runs the cache track and the
// decoder track in lockstep and reports the first per-instruction divergence.
// An oracle that silently fails to spot a divergence is worse than no oracle,
// so this test verifies both directions:
//
//   - a matching workload produces zero divergence reports, and
//   - a deliberately injected cache-track defect IS reported.
//
// The custom DivergenceHandler path is exercised by both cases: a recording
// handler is installed, and the test asserts on whether it was invoked.
//
// Hand-encoded instruction words, verified with an external assembler at
// authorship time (see docs/testing.md).
// =============================================================================

#include "gaby_vm/shadow_runner.h"

#include <array>
#include <cstdint>
#include <cstdio>

#include "gaby_vm/predecode_cache.h"

namespace {

// AArch64 encodings (verified with an external assembler at authorship time).
constexpr uint32_t kAddX0X0_1 = 0x91000400u;  // add x0, x0, #1
constexpr uint32_t kOrrX1X1X2 = 0xaa020021u;  // orr x1, x1, x2
constexpr uint32_t kSubX3X3_5 = 0xd1001463u;  // sub x3, x3, #5
constexpr uint32_t kOrrX0X0X1 = 0xaa010000u;  // orr x0, x0, x1
constexpr uint32_t kRet = 0xd65f03c0u;        // ret

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

struct StackBuffer {
  alignas(16) std::array<uint8_t, 16 * 1024> bytes{};
  void* data() { return bytes.data(); }
  size_t size() const { return bytes.size(); }
};

// A matching workload: both tracks run identical, correctly-cached code, so
// every lockstep step agrees and ShadowRunner reports nothing.
void test_matching_workload_reports_no_divergence() {
  alignas(uint32_t)
      uint32_t code[] = {kAddX0X0_1, kOrrX1X1X2, kSubX3X3_5, kRet};
  gaby_vm::PredecodeCache cache;
  check(cache.RegisterCodeRange(code, sizeof(code)) ==
            gaby_vm::PredecodeCache::RegistrationStatus::Ok,
        "matching: code range registers Ok");

  StackBuffer stack;
  gaby_vm::testing::ShadowRunner shadow(&cache, stack.data(), stack.size());

  int report_count = 0;
  shadow.SetDivergenceHandler(
      [&report_count](const gaby_vm::testing::DivergenceReport&) {
        ++report_count;
      });

  shadow.WriteXRegister(0, 10);
  shadow.WriteXRegister(1, 0x0f);
  shadow.WriteXRegister(2, 0xf0);
  shadow.WriteXRegister(3, 100);
  shadow.RunFrom(reinterpret_cast<uintptr_t>(code));

  check(report_count == 0,
        "matching: ShadowRunner reports zero divergence on a matching run");
}

// An injected cache-track defect. The range is registered, predecoding code[0]
// as ORR (a logical-shifted-register form). code[0] is then overwritten with a
// different-form instruction (ADD immediate). The cache entry is now stale:
// the cache track dispatches the ORR leaf on an ADD word, while the decoder
// track decodes the ADD correctly — so the two tracks must diverge, and
// ShadowRunner must report it. An undetected divergence here would mean the
// oracle itself is broken.
void test_injected_defect_is_detected() {
  alignas(uint32_t) uint32_t code[] = {kOrrX0X0X1, kRet};
  gaby_vm::PredecodeCache cache;
  check(cache.RegisterCodeRange(code, sizeof(code)) ==
            gaby_vm::PredecodeCache::RegistrationStatus::Ok,
        "injected: code range registers Ok");

  // Inject the defect: the predecode pass has already cached code[0] as ORR;
  // overwrite the word so the decoder track will now decode it as ADD.
  code[0] = kAddX0X0_1;

  StackBuffer stack;
  gaby_vm::testing::ShadowRunner shadow(&cache, stack.data(), stack.size());

  int report_count = 0;
  gaby_vm::testing::DivergenceReport captured{};
  shadow.SetDivergenceHandler(
      [&report_count,
       &captured](const gaby_vm::testing::DivergenceReport& report) {
        ++report_count;
        captured = report;
      });

  shadow.WriteXRegister(0, 0);
  shadow.WriteXRegister(1, 0);
  shadow.RunFrom(reinterpret_cast<uintptr_t>(code));

  check(report_count >= 1,
        "injected: ShadowRunner detected the injected cache-track defect");
  check(report_count >= 1 &&
            (captured.pc == reinterpret_cast<uintptr_t>(&code[0])),
        "injected: the divergence report names the defective instruction");
}

}  // namespace

int main() {
  test_matching_workload_reports_no_divergence();
  test_injected_defect_is_detected();

  std::printf("shadow_runner: %d/%d checks passed\n", g_passed, g_total);
  return (g_passed == g_total) ? 0 : 1;
}
