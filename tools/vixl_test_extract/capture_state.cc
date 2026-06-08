// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
#include "capture_state.h"

namespace gaby_vm::extract {

namespace {
CapturedCase g_current;
}

CapturedCase& Current() { return g_current; }

void ResetCurrent(const char* name) {
  g_current = CapturedCase{};
  g_current.name = name;
}

}  // namespace gaby_vm::extract
