// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Authorship-time entry point. Walks the VIXL Test linked list (every TEST()
// the linked .cc registered), replays each body through the capture macros,
// filters out cases gaby-vm cannot execute, and writes the committed fixture
// .inc + manifest. NEVER part of the gaby-vm build/runtime.
#include <cstdio>
#include <cstring>
#include <string>

#include "capture_state.h"
#include "fixture_writer.h"
#include "test-runner.h"  // vixl::Test

namespace {

// CPU-feature name substrings gaby-vm cannot execute (the VisitUnimplemented
// cluster of the design's §7). A captured case whose seen-feature set mentions
// any of these is skipped. Refined empirically as the bulk phases surface more.
const char* kDenyFeatureSubstrings[] = {
    "MTE",
    "BF16",
    "BFloat",
    "TME",
    "WFXT",
    "GCS",
};

}  // namespace

int main(int argc, char** argv) {
  // argv[1] = output .inc path; argv[2] = symbol prefix (e.g. "Integer").
  const std::string out_path = (argc > 1) ? argv[1] : "out.inc";
  const std::string prefix = (argc > 2) ? argv[2] : "Generated";

  gaby_vm::extract::FixtureWriter writer(out_path, prefix);

  int total = 0;
  for (vixl::Test* t = vixl::Test::first(); t != nullptr; t = t->next()) {
    ++total;
    gaby_vm::extract::ResetCurrent(t->name());
    t->run();  // invokes the TEST body -> capture macros fill Current()
    gaby_vm::extract::CapturedCase& c = gaby_vm::extract::Current();

    if (!c.skipped) {
      for (const char* deny : kDenyFeatureSubstrings) {
        if (c.required_features.find(deny) != std::string::npos) {
          c.skipped = true;
          c.skip_reason = std::string("unsupported feature: ") + deny;
          break;
        }
      }
    }
    writer.Add(c);
  }

  int included = writer.Finish();
  std::printf("vixl_test_extract: %d TEST(s) walked, %d included for '%s'\n",
              total,
              included,
              prefix.c_str());
  return (included >= 0) ? 0 : 1;
}
