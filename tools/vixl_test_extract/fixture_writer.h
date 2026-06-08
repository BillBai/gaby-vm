// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Authorship-time only. Serialises captured cases into a committed *.inc of
// constexpr C++ data (consumed by the replay runner) plus a human-readable
// manifest_<family>.md listing what was included and what was dropped, and why.
#ifndef GABY_VM_TOOLS_VIXL_TEST_EXTRACT_FIXTURE_WRITER_H_
#define GABY_VM_TOOLS_VIXL_TEST_EXTRACT_FIXTURE_WRITER_H_

#include <string>
#include <vector>

#include "capture_state.h"

namespace gaby_vm::extract {

class FixtureWriter {
 public:
  // inc_path: output .inc (e.g. test/vixl_port/generated/integer_fixtures.inc).
  // prefix:   symbol prefix and Fixtures() base name (e.g. "Integer").
  FixtureWriter(std::string inc_path, std::string prefix);

  // Classify and stash one captured case (included vs skipped).
  void Add(const CapturedCase& c);

  // Emit the .inc and the sibling manifest_<family>.md. Returns the included
  // case count.
  int Finish();

 private:
  std::string inc_path_;
  std::string prefix_;
  std::vector<CapturedCase> included_;
  std::vector<CapturedCase> skipped_;
};

}  // namespace gaby_vm::extract

#endif  // GABY_VM_TOOLS_VIXL_TEST_EXTRACT_FIXTURE_WRITER_H_
