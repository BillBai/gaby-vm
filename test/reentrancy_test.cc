// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// reentrancy_test: gaby_vm::Simulator dual-track termination + re-entrancy.
//
// Covers two predecode-cache-core tasks:
//   - 4.6  A NOP;RET sequence terminates cleanly through RunFrom (cache track)
//          and DebugRunFrom (decoder track), including on a null-cache
//          Simulator.
//   - 4.5  A nested execution call made from within a leaf restores the
//          enclosing run's interpreter cursor, so the enclosing run resumes
//          intact, while the nested call's register effects flow back through.
//          Covered for all three nested-entry forms: RunFrom, DebugRunFrom and
//          StepOnce(entry_pc).
//
// Hand-encoded instruction words; each is annotated with its mnemonic and was
// checked with an external assembler at authorship time (the build and runtime
// pull in no assembler — see docs/testing.md).
//
// Why the nesting point is a store. A store instruction's leaf does not touch
// the PC, so the outer ExecuteInstructionCached's trailing IncrementPc()
// depends on the cursor being correct. If a nested run's final pc_ (NULL)
// leaked back to the enclosing run, that run would terminate one instruction
// early — which is exactly what the X3 assertion below would catch. Nesting
// from a branch leaf instead would be masked: the branch leaf re-sets the PC
// itself, hiding a broken cursor restore.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstdio>

#include "embedding_stack.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/simulator.h"

namespace {

// AArch64 encodings (verified with an external assembler at authorship time).
constexpr uint32_t kNop = 0xd503201fu;        // nop
constexpr uint32_t kRet = 0xd65f03c0u;        // ret              (ret x30)
constexpr uint32_t kAddX0X0_1 = 0x91000400u;  // add x0, x0, #1
constexpr uint32_t kAddX3X3_1 = 0x91000463u;  // add x3, x3, #1
constexpr uint32_t kStrX2X1 = 0xf9000022u;    // str x2, [x1]

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

// --- task 4.6: NOP;RET terminates cleanly on both tracks ---------------------

void test_nop_ret_terminates() {
  alignas(uint32_t) uint32_t code[] = {kNop, kRet};
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  gaby_vm::PredecodeCache cache;
  check(cache.RegisterCodeRange(code, sizeof(code)) ==
            gaby_vm::PredecodeCache::RegistrationStatus::Ok,
        "NOP;RET range registers Ok");

  // Cache track. Reaching the line after RunFrom means the run terminated
  // (RET to the NULL link register hit the end-of-sim sentinel); a hang would
  // time CTest out, a crash would exit non-zero.
  {
    StackBuffer stack;
    gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
    sim.RunFrom(entry);
    check(true, "cache-track RunFrom(NOP;RET) terminates");
  }
  // Debug track on a cache-backed Simulator.
  {
    StackBuffer stack;
    gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
    sim.DebugRunFrom(entry);
    check(true, "debug-track DebugRunFrom(NOP;RET) terminates");
  }
  // Debug track on a null-cache Simulator (it has no cache track at all).
  {
    StackBuffer stack;
    gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());
    sim.DebugRunFrom(entry);
    check(true, "null-cache DebugRunFrom(NOP;RET) terminates");
  }
}

// --- task 4.5: a nested execution call restores the enclosing cursor ---------
//
// Outer code:  STR x2,[x1] ; ADD x3,x3,#1 ; RET
// Inner code:  ADD x0,x0,#1 ; RET
//
// The outer STR fires a memory-write observer — i.e. a callback running inside
// the store leaf, mid-instruction. The observer nests one execution call over
// the inner code; `NestMode` selects which nested-entry form it uses, and all
// three must leave the enclosing run's cursor intact.

// How the store observer makes its one nested execution call.
enum class NestMode {
  kCacheRun,   // nested RunFrom      — cache track, runs to termination
  kDebugRun,   // nested DebugRunFrom — debug track, runs to termination
  kCacheStep,  // nested StepOnce(pc) — cache track, exactly one instruction
};

void run_nesting_case(NestMode mode, const char* label_suffix) {
  alignas(uint32_t) uint32_t outer[] = {kStrX2X1, kAddX3X3_1, kRet};
  alignas(uint32_t) uint32_t inner[] = {kAddX0X0_1, kRet};

  gaby_vm::PredecodeCache cache;
  bool ok_outer = cache.RegisterCodeRange(outer, sizeof(outer)) ==
                  gaby_vm::PredecodeCache::RegistrationStatus::Ok;
  bool ok_inner = cache.RegisterCodeRange(inner, sizeof(inner)) ==
                  gaby_vm::PredecodeCache::RegistrationStatus::Ok;
  check(ok_outer && ok_inner, "nesting: outer + inner ranges register Ok");

  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  // Scratch cell the outer STR writes into.
  alignas(uint64_t) uint64_t store_target = 0;
  const uintptr_t inner_entry = reinterpret_cast<uintptr_t>(inner);

  bool observer_fired = false;
  bool nested_once = false;
  sim.SetMemoryWriteObserver([&](const gaby_vm::Simulator::MemoryWriteEvent&) {
    observer_fired = true;
    if (nested_once) {
      return;  // nest exactly once
    }
    nested_once = true;
    // Set X30 = 0: a run-to-termination nested call needs it so the inner RET
    // hits the end-of-sim sentinel, and the enclosing run needs it so its own
    // trailing RET terminates.
    sim.Write(gaby_vm::GpRegister::X30, 0);
    switch (mode) {
      case NestMode::kCacheRun:
        sim.RunFrom(inner_entry);  // <-- nested call
        break;
      case NestMode::kDebugRun:
        sim.DebugRunFrom(inner_entry);  // <-- nested call
        break;
      case NestMode::kCacheStep:
        // Re-entrancy-safe single step: StepOnce(entry_pc) seats the nested
        // PC inside the re-entrancy guard. Runs just the inner ADD x0,x0,#1.
        sim.StepOnce(inner_entry);  // <-- nested call
        break;
    }
  });

  sim.Write(gaby_vm::GpRegister::X0, 100);  // inner ADD takes this to 101
  sim.Write(gaby_vm::GpRegister::X1, reinterpret_cast<uint64_t>(&store_target));
  sim.Write(gaby_vm::GpRegister::X2,
            0xfeedfaceu);                 // value the outer STR writes
  sim.Write(gaby_vm::GpRegister::X3, 7);  // outer ADD takes this to 8

  sim.RunFrom(reinterpret_cast<uintptr_t>(outer));

  char label[96];
  std::snprintf(label,
                sizeof(label),
                "nesting (%s): store observer fired",
                label_suffix);
  check(observer_fired, label);

  std::snprintf(label,
                sizeof(label),
                "nesting (%s): X0 == 101 — nested run's register write flowed "
                "back",
                label_suffix);
  check(sim.Read(gaby_vm::GpRegister::X0) == 101, label);

  std::snprintf(label,
                sizeof(label),
                "nesting (%s): X3 == 8 — enclosing run resumed past the nested "
                "call",
                label_suffix);
  check(sim.Read(gaby_vm::GpRegister::X3) == 8, label);

  std::snprintf(label,
                sizeof(label),
                "nesting (%s): outer STR wrote the scratch cell",
                label_suffix);
  check(store_target == 0xfeedfaceu, label);
}

}  // namespace

int main() {
  test_nop_ret_terminates();
  run_nesting_case(NestMode::kCacheRun, "nested cache RunFrom");
  run_nesting_case(NestMode::kDebugRun, "nested debug RunFrom");
  run_nesting_case(NestMode::kCacheStep, "nested cache StepOnce");

  std::printf("reentrancy: %d/%d checks passed\n", g_passed, g_total);
  return (g_passed == g_total) ? 0 : 1;
}
