// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Authorship-time only. The extraction tool links the reference VIXL (Tier-0
// MacroAssembler + Simulator). This file and the rest of
// tools/vixl_test_extract are NEVER part of the gaby-vm build/runtime — they
// run once, offline, to regenerate the committed fixtures under
// test/vixl_port/generated/.
//
// capture_state holds the single in-flight CapturedCase the capture macros
// fill while a VIXL TEST() body is replayed. It is deliberately free of any
// vixl:: type so it can be shared with the fixture writer.
#ifndef GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_STATE_H_
#define GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_STATE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace gaby_vm::extract {

// Mirrors gaby_vm::vixl_port::AssertKind (kept local so this tool does not need
// the gaby_vm headers; it links VIXL, not gaby_vm). The fixture writer maps
// these onto the public enum names when it emits the .inc.
enum class AssertKind : uint8_t { kX, kW, kNZCV, kFP32, kFP64, kV128 };

struct AssertTarget {
  AssertKind kind;
  uint8_t reg;
  uint64_t expected_lo;
  uint64_t expected_hi;
};

// Mirror of gaby_vm::RegisterFile's value fields. This is the canonical
// architectural state at body entry (VIXL's post-ResetState snapshot). sp and
// the LR slot (x[30]) are left 0: the replay runner overrides them, so baking
// VIXL host addresses here would be both pointless and non-reproducible.
struct EntryState {
  uint64_t x[31] = {};
  uint64_t sp = 0;
  uint64_t v_lo[32] = {};
  uint64_t v_hi[32] = {};
  uint32_t nzcv = 0;
  uint32_t fpcr = 0;
  uint32_t fpsr = 0;
};

struct CapturedCase {
  std::string name;
  std::vector<uint32_t> body_words;  // body only (no prologue, no epilogue)
  EntryState entry;
  std::vector<AssertTarget> asserts;
  std::string
      required_features;    // human-readable, from Simulator::GetSeenFeatures
  int run_count = 0;        // RUN() invocations; >1 means we cannot port it
  int dropped_asserts = 0;  // assert forms we could not turn into a target
  bool skipped = false;
  std::string skip_reason;
};

// The single in-flight case being captured by the macros. extract_main resets
// it before invoking each TEST callback, then reads it out.
CapturedCase& Current();
void ResetCurrent(const char* name);

}  // namespace gaby_vm::extract

#endif  // GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_STATE_H_
