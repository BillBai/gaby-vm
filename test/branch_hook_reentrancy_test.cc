// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// branch_hook_reentrancy_test: re-entrancy contract for the branch hook.
//
// Covers the spec scenario `Branch hook may re-enter the simulator under the
// documented contract` and its two sub-scenarios (Read/Write inside the hook
// + nested RunFrom from inside the hook). The contract reuses the existing
// memory-write-observer re-entrancy machinery — the test pins down the
// working pattern so future refactors notice if it stops working.
//
// Hand-encoded uint32_t instruction words; verified at authorship time with
// an external assembler.
// =============================================================================

#include <cstdint>
#include <cstdio>

#include "embedding_stack.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

namespace {

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

void check_eq_u64(uint64_t actual, uint64_t expected, const char* label) {
  g_total += 1;
  if (actual == expected) {
    g_passed += 1;
    return;
  }
  std::fprintf(stderr,
               "[FAIL] %s\n"
               "  actual   = 0x%016llx\n"
               "  expected = 0x%016llx\n",
               label,
               static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(expected));
}

// ---- Scenario 1: hook reads + writes architectural register state ----------
//
// Outer:  ADD X3,X3,#1 ; RET   (simple two-instruction body)
// Hook (fired on the implicit RET that terminates the outer run) reads X3,
// then writes X0 = X3 + 100. Returns target_pc unchanged so the RET commits
// normally.
//
// After the run: X0 must equal the value the hook wrote.

struct ReadWriteCtx {
  uint64_t observed_x3 = 0;
  bool fired = false;
};

uintptr_t read_write_hook(uintptr_t target_pc,
                          void* ud,
                          gaby_vm::Simulator& sim) {
  auto* c = static_cast<ReadWriteCtx*>(ud);
  if (!c->fired) {
    c->fired = true;
    c->observed_x3 = sim.Read(gaby_vm::GpRegister::X3);
    sim.Write(gaby_vm::GpRegister::X0, c->observed_x3 + 100);
  }
  return target_pc;
}

void test_hook_reads_and_writes_registers() {
  alignas(uint32_t) uint32_t code[] = {
      0x91000463,  // add x3, x3, #1
      0xd65f03c0,  // ret
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  gaby_vm::PredecodeCache cache;
  cache.RegisterCodeRange(code, sizeof(code));
  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  ReadWriteCtx ctx;
  sim.SetBranchHook(read_write_hook, &ctx);
  sim.Write(gaby_vm::GpRegister::X3, 7);  // after ADD, X3 = 8
  sim.RunFrom(entry);

  check(ctx.fired, "hook fired at least once during the run");
  check_eq_u64(ctx.observed_x3,
               8,
               "hook observed X3 == 8 (post-ADD architectural state)");
  check_eq_u64(sim.Read(gaby_vm::GpRegister::X0),
               108,
               "register write inside hook persists: X0 = 8 + 100 = 108");
}

// ---- Scenario 2: hook seats a nested RunFrom -------------------------------
//
// Outer:  ADD X3,X3,#1 ; STR X2,[X1] ; ADD X3,X3,#1 ; RET
// Inner:  ADD X0,X0,#1 ; RET
//
// Hook (fires on the implicit terminating RET only) reads LR, performs a
// nested RunFrom(inner_entry) with X30=0 seated so the inner RET terminates
// the inner run, then returns target_pc unchanged so the outer RET commits
// normally.
//
// Why this pattern works for free: ExecutionScope snapshots the outer
// interpreter cursor on nested entry and restores it on exit. The outer run
// resumes correctly.
//
// Checks:
//   - inner run's register effect (X0 += 1) is visible after the outer run;
//   - outer run's pre-hook state (X3 advanced twice through the ADD/STR/ADD)
//     persists;
//   - the outer STR fired its memory write (a non-hook side-effect that the
//     enclosing run must have committed).
//
// Why nest from a RET (not from the outer STR's memory-write observer): we
// want the BRANCH hook's re-entrancy to be the thing under test here. The
// existing reentrancy_test already covers the memory-write observer's
// re-entry path through nested RunFrom.

struct NestCtx {
  uintptr_t inner_entry = 0;
  bool nested = false;
  uint64_t saved_lr = 0;
  uint64_t saved_x30_after_nested = 0;
};

uintptr_t nesting_hook(uintptr_t target_pc, void* ud, gaby_vm::Simulator& sim) {
  auto* c = static_cast<NestCtx*>(ud);
  if (!c->nested) {
    c->nested = true;
    // The outer RET sees LR == 0 (the null-LR sentinel) — that's how the
    // outer run is supposed to terminate. Confirm we observe it.
    c->saved_lr = sim.Read(gaby_vm::GpRegister::LR);
    // Seat X30=0 for the nested call so the inner RET terminates the nested
    // run, and then RunFrom returns cleanly to the hook body.
    sim.Write(gaby_vm::GpRegister::X30, 0);
    sim.RunFrom(c->inner_entry);
    // After the nested run terminates, LR remains 0 (the inner RET committed
    // null to PC). Record it so the test can confirm.
    c->saved_x30_after_nested = sim.Read(gaby_vm::GpRegister::X30);
  }
  return target_pc;
}

void test_hook_seats_nested_run_from() {
  alignas(uint32_t) uint32_t outer[] = {
      0x91000463,  // [0]  add x3, x3, #1
      0xf9000022,  // [4]  str x2, [x1]
      0x91000463,  // [8]  add x3, x3, #1
      0xd65f03c0,  // [12] ret
  };
  alignas(uint32_t) uint32_t inner[] = {
      0x91000400,  // [0] add x0, x0, #1
      0xd65f03c0,  // [4] ret
  };

  gaby_vm::PredecodeCache cache;
  cache.RegisterCodeRange(outer, sizeof(outer));
  cache.RegisterCodeRange(inner, sizeof(inner));
  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  alignas(uint64_t) uint64_t store_target = 0;

  NestCtx ctx;
  ctx.inner_entry = reinterpret_cast<uintptr_t>(inner);
  sim.SetBranchHook(nesting_hook, &ctx);

  sim.Write(gaby_vm::GpRegister::X0, 100);  // inner ADD: 100 -> 101
  sim.Write(gaby_vm::GpRegister::X1, reinterpret_cast<uint64_t>(&store_target));
  sim.Write(gaby_vm::GpRegister::X2, 0xfeedfaceu);  // outer STR writes this
  sim.Write(gaby_vm::GpRegister::X3, 7);            // outer ADD/ADD: 7 -> 9

  sim.RunFrom(reinterpret_cast<uintptr_t>(outer));

  check(ctx.nested, "hook fired and nested run was seated");
  check_eq_u64(ctx.saved_lr,
               0,
               "hook observed LR == 0 (outer's null-LR sentinel)");
  check_eq_u64(sim.Read(gaby_vm::GpRegister::X0),
               101,
               "X0 == 101 — nested run's ADD x0,x0,#1 flowed back through the "
               "hook into the outer Simulator");
  check_eq_u64(sim.Read(gaby_vm::GpRegister::X3),
               9,
               "X3 == 9 — outer ADD/STR/ADD all committed before the hook "
               "fired "
               "on the terminating RET");
  check_eq_u64(store_target, 0xfeedfaceu, "outer STR wrote the scratch cell");
}

// ---- Scenario 3: nested run from an indirect branch preserves outer BType --
//
// Regression test for the branch-hook-api interpreter-cursor fix. A
// register-indirect branch (here BR) stages PSTATE.BTYPE for its target via
// WriteNextBType *before* the branch hook fires. If the hook seats a nested
// run, the nested run's per-instruction UpdateBType() consumes and clears that
// staged value, so the interpreter cursor MUST save/restore btype_/next_btype_
// across the nested run — otherwise the outer branch's target lands with a
// BType corrupted by the nested run.
//
// Asserted differentially: run the SAME guest twice with a hook that records
// BType at the post-BR target on its second fire — once seating a nested run,
// once not. The cursor fix makes the two recordings identical; without it the
// nested run leaks a different value through.
//
// Guest (3 words registered):
//   [0]  br  x4   ; x4 seeded = entry + 8; fire #1 (optionally nests)
//   [4]  nop      ; padding; never executed (BR jumps straight to [8])
//   [8]  ret      ; fire #2: record BType, then terminate (hook returns 0)
// Inner (for the nested run): add x0,x0,#1 ; ret  (X30 = 0 seated ->
// terminates)

struct BTypeCtx {
  uintptr_t inner_entry = 0;
  bool do_nest = false;
  bool in_nested = false;
  int outer_fires = 0;
  uint64_t btype_at_target = 0xffffffffu;  // sentinel: hook never recorded
};

uintptr_t btype_probe_hook(uintptr_t target_pc,
                           void* ud,
                           gaby_vm::Simulator& sim) {
  auto* c = static_cast<BTypeCtx*>(ud);
  // Inner-run branches (the nested ADD/RET) pass through untouched — they must
  // not record or re-nest. The inner RET reads X30 == 0, so returning
  // target_pc commits 0 and terminates the nested run.
  if (c->in_nested) {
    return target_pc;
  }
  c->outer_fires += 1;
  if (c->outer_fires == 1) {
    // The outer BR. Optionally seat one nested run, then commit to the target.
    if (c->do_nest) {
      c->in_nested = true;
      sim.Write(gaby_vm::GpRegister::X30, 0);
      sim.RunFrom(c->inner_entry);
      c->in_nested = false;
    }
    return target_pc;
  }
  // fire #2: the target RET. UpdateBType() at the start of this instruction has
  // already promoted the BR's staged next_btype_ into btype_, so this reads the
  // BType the target actually saw. Record it, then terminate the run.
  c->btype_at_target = sim.Read(gaby_vm::SysRegister::BType);
  return 0;
}

uint64_t run_btype_probe(bool do_nest) {
  alignas(uint32_t) uint32_t outer[] = {
      0xd61f0080,  // [0] br x4   (x4 = entry + 8)
      0xd503201f,  // [4] nop     (padding; never executed)
      0xd65f03c0,  // [8] ret
  };
  alignas(uint32_t) uint32_t inner[] = {
      0x91000400,  // [0] add x0, x0, #1
      0xd65f03c0,  // [4] ret
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(outer);

  gaby_vm::PredecodeCache cache;
  cache.RegisterCodeRange(outer, sizeof(outer));
  cache.RegisterCodeRange(inner, sizeof(inner));
  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  BTypeCtx ctx;
  ctx.inner_entry = reinterpret_cast<uintptr_t>(inner);
  ctx.do_nest = do_nest;
  sim.SetBranchHook(btype_probe_hook, &ctx);

  sim.Write(gaby_vm::GpRegister::X4, entry + 8);  // BR target = the RET at [8]
  sim.RunFrom(entry);

  check(ctx.outer_fires == 2, "btype probe: hook fired on BR and target RET");
  return ctx.btype_at_target;
}

void test_nested_run_preserves_outer_btype() {
  const uint64_t without_nest = run_btype_probe(/*do_nest=*/false);
  const uint64_t with_nest = run_btype_probe(/*do_nest=*/true);
  check_eq_u64(with_nest,
               without_nest,
               "BR target's BType is identical whether or not the hook seated "
               "a nested run (cursor saves/restores btype_/next_btype_)");
}

}  // namespace

int main() {
  test_hook_reads_and_writes_registers();
  test_hook_seats_nested_run_from();
  test_nested_run_preserves_outer_btype();

  std::printf("branch_hook_reentrancy: %d/%d checks passed\n",
              g_passed,
              g_total);
  return (g_passed == g_total) ? 0 : 1;
}
