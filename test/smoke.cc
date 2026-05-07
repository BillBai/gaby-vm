// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#include <cstring>
#include <iostream>

#include "gaby_vm/gaby_vm.h"

// Smoke test: asserts the public API surface is reachable, the static library
// links, and version() returns a non-empty string. Intentionally minimal — the
// goal is to fail fast if the build pipeline is broken, not to exercise
// behaviour.

int main() {
  const char* v = gaby_vm::version();
  if (v == nullptr || std::strlen(v) == 0) {
    std::cerr << "FAIL: gaby_vm::version() returned empty\n";
    return 1;
  }
  std::cout << "gaby_vm::version() = " << v << "\n";
  return 0;
}
