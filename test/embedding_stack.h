// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// Shared embedding-stack helper for the test suite: a 16 KiB, 16-byte-aligned
// byte buffer exposing the data()/size() shape gaby_vm::Simulator's
// constructor wants. Extracted so the tests have one definition instead of a
// byte-identical copy per file.
//
// The bench and the CLI demo allocate their stacks with
// std::vector<uint8_t>(gaby_vm::Simulator::kMinStackSize) instead; the tests
// keep this fixed-size struct because several of them seat nested runs and
// want headroom above the documented floor.

#ifndef GABY_VM_TEST_EMBEDDING_STACK_H_
#define GABY_VM_TEST_EMBEDDING_STACK_H_

#include <array>
#include <cstddef>
#include <cstdint>

struct StackBuffer {
  alignas(16) std::array<uint8_t, 16 * 1024> bytes{};
  void* data() { return bytes.data(); }
  size_t size() const { return bytes.size(); }
};

#endif  // GABY_VM_TEST_EMBEDDING_STACK_H_
