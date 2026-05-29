// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// branch_hook_dispatch_test: gaby_vm::Simulator branch-hook firing contract.
//
// Covers the spec's per-branch-family firing scenarios + hook return-value
// semantics + dual-track equivalence for `aarch64-simulator: Branch hook
// fires on every taken PC-redirecting branch ...` and `BranchHook return
// value chooses between continue, divert, and terminate` (openspec/changes/
// branch-hook-api/specs/aarch64-simulator/spec.md).
//
// Each guest code fragment ends with `RET` so it terminates through the
// imported simulator's null-LR sentinel — every RET fire counts as one hook
// invocation with `target_pc == 0` (the null sentinel), the same on both
// tracks. Tests that care about which fire corresponds to which branch index
// into the recorded list explicitly.
//
// Hand-encoded uint32_t instruction words; verified at authorship time with
// an external assembler (see simulator_correctness.cc for the exact
// recipe). The build and runtime pull in no assembler.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

namespace {

// ---- Recording context the hook writes into --------------------------------

struct Fire {
  uintptr_t target_pc;
  uint64_t lr_at_fire;
};

struct Recorder {
  std::vector<Fire> fires;
};

// Identity-action hook: records (target_pc, LR) and returns target_pc so the
// architectural branch is committed unchanged.
uintptr_t record_identity(uintptr_t target_pc,
                          void* ud,
                          gaby_vm::Simulator& sim) {
  auto* r = static_cast<Recorder*>(ud);
  r->fires.push_back({target_pc, sim.Read(gaby_vm::GpRegister::LR)});
  return target_pc;
}

// ---- Test harness ----------------------------------------------------------

int g_passed = 0;
int g_total = 0;
const char* g_subtest = "";
const char* g_track = "";

void check(bool ok, const char* label) {
  g_total += 1;
  if (ok) {
    g_passed += 1;
    return;
  }
  std::fprintf(stderr, "[FAIL] %s / %s track: %s\n", g_subtest, g_track, label);
}

void check_eq_u64(uint64_t actual, uint64_t expected, const char* label) {
  g_total += 1;
  if (actual == expected) {
    g_passed += 1;
    return;
  }
  std::fprintf(stderr,
               "[FAIL] %s / %s track: %s\n"
               "  actual   = 0x%016llx\n"
               "  expected = 0x%016llx\n",
               g_subtest,
               g_track,
               label,
               static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(expected));
}

struct StackBuffer {
  alignas(16) std::array<uint8_t, 16 * 1024> bytes{};
  void* data() { return bytes.data(); }
  size_t size() const { return bytes.size(); }
};

using SetupFn = std::function<void(gaby_vm::Simulator&)>;

// Run `code` through the cache track with an identity recording hook
// installed. `setup` writes initial register state before RunFrom.
Recorder run_cache(const uint32_t* code,
                   size_t code_words,
                   const SetupFn& setup) {
  gaby_vm::PredecodeCache cache;
  const auto status =
      cache.RegisterCodeRange(code, code_words * sizeof(uint32_t));
  if (status != gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    std::fprintf(stderr, "  RegisterCodeRange failed\n");
  }
  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
  Recorder rec;
  sim.SetBranchHook(record_identity, &rec);
  if (setup) {
    setup(sim);
  }
  sim.RunFrom(reinterpret_cast<uintptr_t>(code));
  return rec;
}

// Run `code` through the decoder track. The decoder track does not require a
// cache, but the hook is installed all the same.
Recorder run_decoder(const uint32_t* code,
                     size_t code_words,
                     const SetupFn& setup) {
  (void)code_words;
  StackBuffer stack;
  gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());
  Recorder rec;
  sim.SetBranchHook(record_identity, &rec);
  if (setup) {
    setup(sim);
  }
  sim.DebugRunFrom(reinterpret_cast<uintptr_t>(code));
  return rec;
}

// Pair of recorders for dual-track scenarios. The dual-track equivalence
// scenario asserts both lists match.
struct DualResult {
  Recorder cache;
  Recorder decoder;
};

DualResult run_dual(const uint32_t* code,
                    size_t code_words,
                    const SetupFn& setup) {
  return {run_cache(code, code_words, setup),
          run_decoder(code, code_words, setup)};
}

// Assert the two tracks recorded the same fires.
void check_track_equivalence(const Recorder& a, const Recorder& b) {
  check(a.fires.size() == b.fires.size(),
        "cache and decoder track recorded equal fire counts");
  const size_t n = std::min(a.fires.size(), b.fires.size());
  for (size_t i = 0; i < n; ++i) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "fire #%zu: target_pc matches", i);
    check_eq_u64(a.fires[i].target_pc, b.fires[i].target_pc, buf);
  }
}

// ---- Per-branch-family scenarios -------------------------------------------

// B +8 ; MOV X0,#0xB0B0 ; RET
//
// Expected fires:
//   #0: B at offset 0 → target_pc = entry + 8 (the RET)
//   #1: RET → target_pc = 0 (null LR sentinel)
void test_b_uncond() {
  g_subtest = "B unconditional";
  alignas(uint32_t) uint32_t code[] = {
      0x14000002,  // b #+8
      0xd2961600,  // mov x0, #0xB0B0
      0xd65f03c0,  // ret
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  auto run = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 2,
          "two fires: the B + the RET that terminates the run");
    if (rec.fires.size() >= 1) {
      check_eq_u64(rec.fires[0].target_pc,
                   entry + 8,
                   "fire #0 (B) target_pc = entry+8");
    }
    if (rec.fires.size() >= 2) {
      check_eq_u64(rec.fires[1].target_pc,
                   0,
                   "fire #1 (RET) target_pc = 0 (null-LR sentinel)");
    }
  };
  const DualResult dual = run_dual(code, 3, SetupFn{});
  run(dual.cache, "cache");
  run(dual.decoder, "decoder");
  g_track = "(both)";
  check_track_equivalence(dual.cache, dual.decoder);
}

// MOV X19,LR ; BL +12 ; MOV LR,X19 ; RET ; <callee:> MOVZ X0,#0xAB1E ; RET
//
// Hook expectations:
//   #0: BL — target_pc = callee (entry + 16); LR observed by hook MUST equal
//       PC+4 of the BL = entry + 8 (the `mov lr, x19` that follows BL).
//   #1: inner RET — target_pc = entry + 8 (the mov lr, x19 we return to).
//   #2: outer RET — target_pc = 0 (the null-LR sentinel restored from X19).
void test_bl_lr() {
  g_subtest = "BL + RET (LR observed in hook == PC+4)";
  alignas(uint32_t) uint32_t code[] = {
      0xaa1e03f3,  // mov x19, lr
      0x94000003,  // bl   +12        (-> callee at offset 16)
      0xaa1303fe,  // mov lr,  x19
      0xd65f03c0,  // ret (outer)
      0xd29563c0,  // movz x0, #0xAB1E
      0xd65f03c0,  // ret (inner)
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  auto run = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 3, "three fires: BL, inner RET, outer RET");
    if (rec.fires.size() >= 1) {
      check_eq_u64(rec.fires[0].target_pc,
                   entry + 16,
                   "fire #0 (BL) target_pc = callee (entry+16)");
      check_eq_u64(rec.fires[0].lr_at_fire,
                   entry + 8,
                   "fire #0 (BL) LR observed in hook == PC+4 of the BL");
    }
    if (rec.fires.size() >= 2) {
      check_eq_u64(rec.fires[1].target_pc,
                   entry + 8,
                   "fire #1 (inner RET) target_pc = post-BL return address");
    }
    if (rec.fires.size() >= 3) {
      check_eq_u64(rec.fires[2].target_pc,
                   0,
                   "fire #2 (outer RET) target_pc = 0 (null-LR sentinel)");
    }
  };
  const DualResult dual = run_dual(code, 6, SetupFn{});
  run(dual.cache, "cache");
  run(dual.decoder, "decoder");
  g_track = "(both)";
  check_track_equivalence(dual.cache, dual.decoder);
}

// SUBS X3,X1,X1 ; B.EQ +8 ; MOV X0,#0xB0B0 ; RET — taken and not-taken cases.
//
// Taken (X1=42 → SUBS Z=1): B.EQ takes; the MOV is skipped; the RET fires.
//   #0: B.EQ → entry + 12
//   #1: RET  → 0
// Not-taken (X1=10,X2=3 → SUBS Z=0): B.EQ falls through; MOV runs; RET fires.
// (Note: the not-taken variant uses SUBS X3,X1,X2, so the encoded code blob
// differs slightly; we only need to demonstrate "not-taken does not fire" so
// we run it on a code blob that always produces Z=0.)
void test_b_cond() {
  g_subtest = "B.cond taken";
  alignas(uint32_t) uint32_t code_taken[] = {
      0xeb010023,  // subs x3, x1, x1   ; Z=1
      0x54000040,  // b.eq #+8
      0xd2961600,  // mov x0, #0xB0B0
      0xd65f03c0,  // ret
  };
  const uintptr_t entry_t = reinterpret_cast<uintptr_t>(code_taken);
  auto run_t = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 2, "two fires: B.EQ taken + RET");
    if (rec.fires.size() >= 1) {
      check_eq_u64(rec.fires[0].target_pc,
                   entry_t + 12,
                   "fire #0 (B.EQ taken) target_pc = entry+12");
    }
    if (rec.fires.size() >= 2) {
      check_eq_u64(rec.fires[1].target_pc, 0, "fire #1 (RET) target_pc = 0");
    }
  };
  const DualResult dual_t = run_dual(code_taken, 4, [](gaby_vm::Simulator&) {});
  run_t(dual_t.cache, "cache");
  run_t(dual_t.decoder, "decoder");

  g_subtest = "B.cond not-taken";
  alignas(uint32_t) uint32_t code_nt[] = {
      0xeb020023,  // subs x3, x1, x2   ; X1=10 - X2=3 → Z=0
      0x54000040,  // b.eq #+8          ; not taken
      0xd2961600,  // mov x0, #0xB0B0
      0xd65f03c0,  // ret
  };
  auto setup_nt = [](gaby_vm::Simulator& sim) {
    sim.Write(gaby_vm::GpRegister::X1, 10);
    sim.Write(gaby_vm::GpRegister::X2, 3);
  };
  auto run_nt = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 1,
          "exactly one fire (the RET) — not-taken B.cond does NOT fire");
    if (rec.fires.size() >= 1) {
      check_eq_u64(rec.fires[0].target_pc, 0, "fire #0 (RET) target_pc = 0");
    }
  };
  const DualResult dual_nt = run_dual(code_nt, 4, setup_nt);
  run_nt(dual_nt.cache, "cache");
  run_nt(dual_nt.decoder, "decoder");
}

// TBZ X1, #0, +8 ; MOV X0,#0xB0B0 ; RET — taken when bit 0 of X1 is 0.
void test_tbz() {
  alignas(uint32_t) uint32_t code[] = {
      0x36000041,  // tbz w1, #0, #+8
      0xd2961600,  // mov x0, #0xB0B0
      0xd65f03c0,  // ret
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  g_subtest = "TBZ taken";
  auto setup_taken = [](gaby_vm::Simulator& sim) {
    sim.Write(gaby_vm::GpRegister::X1, 0);  // bit 0 is 0 → taken
  };
  auto run_t = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 2, "two fires: TBZ taken + RET");
    if (rec.fires.size() >= 1) {
      check_eq_u64(rec.fires[0].target_pc, entry + 8, "TBZ target = entry+8");
    }
  };
  const DualResult dual_t = run_dual(code, 3, setup_taken);
  run_t(dual_t.cache, "cache");
  run_t(dual_t.decoder, "decoder");

  g_subtest = "TBZ not-taken";
  auto setup_nt = [](gaby_vm::Simulator& sim) {
    sim.Write(gaby_vm::GpRegister::X1, 1);  // bit 0 is 1 → not taken
  };
  auto run_nt = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 1, "exactly one fire (RET) — TBZ does NOT fire");
  };
  const DualResult dual_nt = run_dual(code, 3, setup_nt);
  run_nt(dual_nt.cache, "cache");
  run_nt(dual_nt.decoder, "decoder");
}

// CBZ X1, +8 ; MOV X0,#0xB0B0 ; RET — taken when X1 == 0.
void test_cbz() {
  alignas(uint32_t) uint32_t code[] = {
      0xb4000041,  // cbz x1, #+8
      0xd2961600,  // mov x0, #0xB0B0
      0xd65f03c0,  // ret
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  g_subtest = "CBZ taken";
  auto setup_taken = [](gaby_vm::Simulator& sim) {
    sim.Write(gaby_vm::GpRegister::X1, 0);
  };
  auto run_t = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 2, "two fires: CBZ taken + RET");
    if (rec.fires.size() >= 1) {
      check_eq_u64(rec.fires[0].target_pc, entry + 8, "CBZ target = entry+8");
    }
  };
  const DualResult dual_t = run_dual(code, 3, setup_taken);
  run_t(dual_t.cache, "cache");
  run_t(dual_t.decoder, "decoder");

  g_subtest = "CBZ not-taken";
  auto setup_nt = [](gaby_vm::Simulator& sim) {
    sim.Write(gaby_vm::GpRegister::X1, 1);
  };
  auto run_nt = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 1, "exactly one fire (RET) — CBZ does NOT fire");
  };
  const DualResult dual_nt = run_dual(code, 3, setup_nt);
  run_nt(dual_nt.cache, "cache");
  run_nt(dual_nt.decoder, "decoder");
}

// BR X1 — register-indirect branch to a target prepared by the host.
//
// Layout: BR X1 (fires hook, target = X1) — and the simulator continues at the
// chosen PC. We point X1 at the "callee", which is just RET; that fires hook
// #1 with target_pc = 0 (null LR) and terminates.
void test_br() {
  g_subtest = "BR Xn";
  alignas(uint32_t) uint32_t code[] = {
      0xd61f0020,  // br x1
      0xd65f03c0,  // ret  (callee that BR targets)
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);
  auto setup = [&](gaby_vm::Simulator& sim) {
    sim.Write(gaby_vm::GpRegister::X1, entry + 4);  // BR target = the RET
  };

  auto run = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 2,
          "two fires: BR + the callee's RET that terminates");
    if (rec.fires.size() >= 1) {
      check_eq_u64(rec.fires[0].target_pc, entry + 4, "BR target = X1");
    }
  };
  const DualResult dual = run_dual(code, 2, setup);
  run(dual.cache, "cache");
  run(dual.decoder, "decoder");
}

// BLR X1 — register-indirect call. Hook sees the post-BLR LR.
void test_blr() {
  g_subtest = "BLR Xn (LR observed in hook == PC+4)";
  alignas(uint32_t) uint32_t code[] = {
      0xaa1e03f3,  // mov x19, lr      ; save outer LR
      0xd63f0020,  // blr x1
      0xaa1303fe,  // mov lr,  x19     ; restore outer LR
      0xd65f03c0,  // ret              ; outer terminator
      0xd65f03c0,  // ret              ; callee
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);
  auto setup = [&](gaby_vm::Simulator& sim) {
    // Callee at offset 16.
    sim.Write(gaby_vm::GpRegister::X1, entry + 16);
  };

  auto run = [&](Recorder rec, const char* track) {
    g_track = track;
    check(rec.fires.size() == 3, "three fires: BLR + callee RET + outer RET");
    if (rec.fires.size() >= 1) {
      check_eq_u64(rec.fires[0].target_pc,
                   entry + 16,
                   "BLR target = X1 (callee)");
      check_eq_u64(rec.fires[0].lr_at_fire,
                   entry + 8,
                   "BLR LR observed in hook = PC+4 of BLR (mov lr, x19)");
    }
  };
  const DualResult dual = run_dual(code, 5, setup);
  run(dual.cache, "cache");
  run(dual.decoder, "decoder");
}

// ---- Hook return-value semantics: identity, divert, terminate -------------

// Divert: hook returns divert_to on the first fire, then identity for the
// rest. We point divert_to at a different in-range PC.
struct DivertCtx {
  uintptr_t divert_to;
  bool divert_armed;
};

uintptr_t divert_then_identity(uintptr_t target_pc,
                               void* ud,
                               gaby_vm::Simulator&) {
  auto* c = static_cast<DivertCtx*>(ud);
  if (c->divert_armed) {
    c->divert_armed = false;
    return c->divert_to;
  }
  return target_pc;
}

void test_divert_action() {
  g_subtest = "hook return divert";
  alignas(uint32_t) uint32_t code[] = {
      0x14000003,  // [0]  b #+12        ; falls into the "wrong" RET unless we
                   // divert
      0xd2960020,  // [4]  movz x0, #0xB001     ; "wrong" arm
      0xd65f03c0,  // [8]  ret                  ; terminates after "wrong"
      0xd2960040,  // [12] movz x0, #0xB002     ; "right" arm — the
                   // architectural target
      0xd65f03c0,  // [16] ret                  ; terminates after "right"
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  // The architectural target of the B is entry + 12 ("right"). We divert it
  // to entry + 4 ("wrong"). Expect X0 == 0xB001 after the run.
  DivertCtx ctx{entry + 4, true};

  // Cache track.
  {
    g_track = "cache";
    gaby_vm::PredecodeCache cache;
    cache.RegisterCodeRange(code, sizeof(code));
    StackBuffer stack;
    gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
    sim.SetBranchHook(divert_then_identity, &ctx);
    sim.RunFrom(entry);
    check_eq_u64(sim.Read(gaby_vm::GpRegister::X0),
                 0xB001,
                 "X0 = 0xB001 — divert went to the diverted PC");
  }
  // Decoder track.
  {
    g_track = "decoder";
    ctx.divert_armed = true;
    StackBuffer stack;
    gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());
    sim.SetBranchHook(divert_then_identity, &ctx);
    sim.DebugRunFrom(entry);
    check_eq_u64(sim.Read(gaby_vm::GpRegister::X0),
                 0xB001,
                 "X0 = 0xB001 — divert went to the diverted PC");
  }
}

// Terminate: hook returns 0 on the first fire. RunFrom returns; the MOV
// after the B does NOT run; the next StepOnce() returns false. 0 is the
// kEndOfSimAddress sentinel (the same path a guest RET to a null LR
// terminates through).
uintptr_t terminate_on_first_fire(uintptr_t /*target_pc*/,
                                  void* /*ud*/,
                                  gaby_vm::Simulator&) {
  return 0;
}

void test_terminate_action() {
  g_subtest = "hook return 0 terminates";
  alignas(uint32_t) uint32_t code[] = {
      0x14000001,  // [0] b #+4
      0xd2961600,  // [4] mov x0, #0xB0B0    (should NOT run)
      0xd65f03c0,  // [8] ret
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);
  const uint64_t sentinel = 0xA0A0;

  // Cache track.
  {
    g_track = "cache";
    gaby_vm::PredecodeCache cache;
    cache.RegisterCodeRange(code, sizeof(code));
    StackBuffer stack;
    gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
    sim.SetBranchHook(terminate_on_first_fire, nullptr);
    sim.Write(gaby_vm::GpRegister::X0, sentinel);
    sim.RunFrom(entry);
    check_eq_u64(sim.Read(gaby_vm::GpRegister::X0),
                 sentinel,
                 "X0 still == sentinel — MOV after the terminating B did NOT "
                 "run");
    check(sim.StepOnce() == false,
          "next StepOnce() returns false (PC at kEndOfSimAddress)");
  }
  // Decoder track.
  {
    g_track = "decoder";
    StackBuffer stack;
    gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());
    sim.SetBranchHook(terminate_on_first_fire, nullptr);
    sim.Write(gaby_vm::GpRegister::X0, sentinel);
    sim.DebugRunFrom(entry);
    check_eq_u64(sim.Read(gaby_vm::GpRegister::X0),
                 sentinel,
                 "X0 still == sentinel — MOV after the terminating B did NOT "
                 "run");
    check(sim.DebugStepOnce() == false,
          "next DebugStepOnce() returns false (PC at kEndOfSimAddress)");
  }
}

// ---- Null-hook / removal scenarios -----------------------------------------

void test_no_hook_pre_change_behavior() {
  g_subtest = "no hook installed: pre-change behavior preserved";
  alignas(uint32_t) uint32_t code[] = {
      0x14000002,  // b #+8
      0xd2961600,  // mov x0, #0xB0B0
      0xd65f03c0,  // ret
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);
  const uint64_t sentinel = 0xA0A0;

  // Cache track: no SetBranchHook call — should behave exactly as before.
  {
    g_track = "cache";
    gaby_vm::PredecodeCache cache;
    cache.RegisterCodeRange(code, sizeof(code));
    StackBuffer stack;
    gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
    sim.Write(gaby_vm::GpRegister::X0, sentinel);
    sim.RunFrom(entry);
    check_eq_u64(sim.Read(gaby_vm::GpRegister::X0),
                 sentinel,
                 "X0 == sentinel — B took, MOV was skipped");
  }
  // Cache track: SetBranchHook(nullptr, nullptr) removes any hook.
  {
    g_track = "cache (nullptr hook installed)";
    gaby_vm::PredecodeCache cache;
    cache.RegisterCodeRange(code, sizeof(code));
    StackBuffer stack;
    gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
    sim.SetBranchHook(nullptr, nullptr);
    sim.Write(gaby_vm::GpRegister::X0, sentinel);
    sim.RunFrom(entry);
    check_eq_u64(sim.Read(gaby_vm::GpRegister::X0),
                 sentinel,
                 "X0 == sentinel still");
  }
}

void test_replace_hook() {
  g_subtest = "SetBranchHook replaces installed hook";
  alignas(uint32_t) uint32_t code[] = {
      0x14000002,  // b #+8
      0xd2961600,  // mov x0, #0xB0B0
      0xd65f03c0,  // ret
  };
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  struct Counter {
    int fires = 0;
  };
  Counter a;
  Counter b;
  auto count =
      [](uintptr_t target, void* ud, gaby_vm::Simulator&) -> uintptr_t {
    static_cast<Counter*>(ud)->fires += 1;
    return target;
  };

  g_track = "cache";
  gaby_vm::PredecodeCache cache;
  cache.RegisterCodeRange(code, sizeof(code));
  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
  // Install A — then immediately replace with B before any run.
  sim.SetBranchHook(count, &a);
  sim.SetBranchHook(count, &b);
  sim.RunFrom(entry);
  check_eq_u64(static_cast<uint64_t>(a.fires),
               0,
               "replaced hook A: zero fires recorded against A's counter");
  check(b.fires > 0, "active hook B: B's counter saw the fires");
}

}  // namespace

int main() {
  test_b_uncond();
  test_bl_lr();
  test_b_cond();
  test_tbz();
  test_cbz();
  test_br();
  test_blr();
  test_divert_action();
  test_terminate_action();
  test_no_hook_pre_change_behavior();
  test_replace_hook();

  std::printf("branch_hook_dispatch: %d/%d checks passed\n", g_passed, g_total);
  return (g_passed == g_total) ? 0 : 1;
}
