// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// Gaby-authored replacement for VIXL's test/test-utils.cc, which is
// deliberately NOT copied into the island. Upstream test-utils.cc
// unconditionally includes <sys/mman.h> and its ExecuteMemory() mmaps RWX
// memory and *natively calls* the assembled bytes — a direct no-JIT / no-RWX /
// iOS violation.
//
// The vixl_port harness never natively executes assembled code: it feeds the
// assembled bytes to the gaby simulator's decode path. So the only symbol the
// VIXL test infrastructure actually links from that translation unit is
// ExecuteMemory, supplied here as abort-on-call. The exact symbol set is
// whatever the linker reports undefined (expected: ExecuteMemory alone); if a
// future upstream sync references another test-utils.cc symbol, add a stub for
// it here — never resurrect the mmap implementation.

#include <cstdio>
#include <cstdlib>

#include "test-utils.h"

namespace vixl {

void ExecuteMemory(byte* /*buffer*/, size_t /*size*/, int /*byte_offset*/) {
  fprintf(stderr,
          "gaby-vm vixl_port: ExecuteMemory() must never be called — the "
          "test-only assembler island does not natively execute assembled "
          "bytes (no-JIT/no-RWX). Assembled code runs on the gaby simulator's "
          "decode path instead. This is a harness bug.\n");
  abort();
}

}  // namespace vixl
