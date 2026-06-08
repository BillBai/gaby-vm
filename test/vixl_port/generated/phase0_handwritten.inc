// Phase-0 hand-authored fixtures (NOT machine-generated). They exercise the
// replay runner before the extraction tool exists. Phase 1 replaces these with
// the machine-generated integer set and this file is removed.
//
// Convention matches PortedFixture: `code` is the body ONLY (no trailing RET);
// the runner appends the terminating RET. Encodings verified against the
// project's existing hand-encoded sequences (see test/shadow_runner_test.cc).
#include "vixl_port_fixture.h"

namespace gaby_vm::vixl_port {
namespace {

// add x0, x1, x2   (body only; runner appends RET)
constexpr uint32_t kAddCode[] = {0x8b020020u};
constexpr AssertTarget kAddAsserts[] = {
    // entry: X1=2, X2=3  ->  X0 == 5
    {AssertKind::kX, /*reg=*/0, /*lo=*/5u, /*hi=*/0u},
};

RegisterFile MakeAddEntry() {
  RegisterFile rf{};  // value-initialised: all zero
  rf.x[1] = 2u;
  rf.x[2] = 3u;
  return rf;
}

}  // namespace

// Exposed for the integer main. MakeAddEntry() is evaluated once at startup.
inline const PortedFixture* Phase0Fixtures(size_t* count) {
  static const RegisterFile add_entry = MakeAddEntry();
  static const PortedFixture fixtures[] = {
      {"hand/add_x0_x1_x2", kAddCode, 1, add_entry, kAddAsserts, 1},
  };
  *count = sizeof(fixtures) / sizeof(fixtures[0]);
  return fixtures;
}

}  // namespace gaby_vm::vixl_port
