// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Per-family entry point for the live-assemble vixl_port harness. Walks the
// VIXL Test linked list (every TEST() the included upstream .cc registered),
// runs each body through the two-track macros under a crash/hang guard, and
// tallies. One family executable includes the two-track macros, then the
// upstream test .cc, then this header, then calls RunRegisteredTests().
//
// Ported from tools/vixl_test_extract/extract_main.cc: same sigsetjmp + alarm
// crash/hang guard and by-name quarantine; the structural IsNonPortableInstr
// filter is deliberately NOT carried over (load/store/PC-rel bodies run now).
#ifndef GABY_VM_TEST_VIXL_ASM_GABY_TWO_TRACK_MAIN_H_
#define GABY_VM_TEST_VIXL_ASM_GABY_TWO_TRACK_MAIN_H_

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>  // alarm

#include "gaby_two_track_macros.h"
#include "test-runner.h"  // vixl::Test

namespace gaby_vm {
namespace vixl_port_live {

// Tests skipped by name: simulator-runtime / system bodies that call host
// functions, intercept branches, drive the debugger, zero memory by VA, or run
// MOPS sequences. Host-coupled and not portable to a two-track replay. (The old
// structural load/store / PC-rel filter is gone — those bodies run now.)
inline const char* const kQuarantineByName[] = {
    "AARCH64_ASM_printf",
    "AARCH64_ASM_printf_no_preserve",
    "AARCH64_ASM_runtime_calls",
    "AARCH64_ASM_branch_interception",
    "AARCH64_ASM_large_sim_stack",
    "AARCH64_ASM_dc_zva",
    "AARCH64_ASM_system",
    "AARCH64_ASM_generic_operand",
    // gcs_feature_off disables the Guarded-Control-Stack check by calling
    // simulator.DisableGCSCheck() on the REFERENCE sim directly, then runs a
    // Bl/Adr(lr)/Ret sequence that deliberately mismatches the return address —
    // which is only valid with GCS checking off. gaby's two tracks are separate
    // Simulator instances and the gaby public API exposes no GCS-disable, so
    // they keep GCS enabled and abort on that sequence. GCS is out of V1 scope
    // (deny-listed); this is a harness/scope limitation, not a leaf divergence.
    "AARCH64_ASM_gcs_feature_off",
    "AARCH64_ASM_mops_set",
    "AARCH64_ASM_mops_setn",
    "AARCH64_ASM_mops_cpy",
    "AARCH64_ASM_mops_cpyn",
    "AARCH64_ASM_mops_cpyf",
    "AARCH64_ASM_mops_cpyfn",
};

// Tests skipped by name SUBSTRING. Neither is a gaby correctness gap:
//   * configure_cpu_features* tests the feature-config mechanism by restricting
//     the (reused) reference sim's CPUFeatures and asserting on them;
//     host/config coupled, not a leaf-semantics test.
//   * branch_to_reg is a register-indirect branch into BTI-guarded code; the
//     wild-PC / BType enforcement aborts under the seeded no-prologue model.
//
// NOTE: the read-modify-write families (atomics / exclusives / CAS, and the
// NEON store-multiple ST1/2/3/4 bodies) are NOT quarantined. They RMW a
// body-local buffer at a baked address shared by all three engines, but
// TwoTrackRun resets that buffer (the body's stack frame) between runs, so each
// engine starts from identical memory and they run on both tracks under both
// oracles.
inline const char* const kQuarantineSubstrings[] = {
    "configure_cpu_features",
    "branch_to_reg",
};

inline bool IsQuarantined(const char* name) {
  for (const char* q : kQuarantineByName) {
    if (std::strcmp(name, q) == 0) {
      return true;
    }
  }
  std::string n(name);
  for (const char* sub : kQuarantineSubstrings) {
    if (n.find(sub) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Per-test crash/hang guard (extract_main.cc). A fatal signal during t->run()
// longjmps back to the loop, which marks the case skipped and continues.
inline sigjmp_buf g_jmp;
inline volatile sig_atomic_t g_fatal_signal = 0;

// Host-level hang backstop for a body wedged OUTSIDE the simulator loop (the
// in-sim kMaxInstructions cap handles loops with a descriptive skip reason).
inline constexpr unsigned kHostWatchdogSeconds = 20;
inline constexpr unsigned kLongPaddingHostWatchdogSeconds = 120;

inline unsigned HostWatchdogSecondsForCurrentCase() {
  return IsLongPaddingCase() ? kLongPaddingHostWatchdogSeconds
                             : kHostWatchdogSeconds;
}

extern "C" inline void OnFatalSignal(int sig) {
  g_fatal_signal = sig;
  siglongjmp(g_jmp, 1);
}

inline void InstallGuards() {
  // Run the handler on an alternate signal stack: a gaby-track wild PC or a
  // runaway recursion can exhaust the host stack, and the resulting SIGSEGV
  // would re-fault the handler itself on the dead stack (so the process dies
  // before it can siglongjmp out, turning a catchable gaby-track crash into an
  // uncontained abort). SA_ONSTACK below makes the handler use this stack
  // instead. The handler only sets a flag and longjmps, so a small stack is
  // ample; if sigaltstack fails the handler simply runs on the normal stack as
  // before (graceful degradation).
  static char alt_stack[64 * 1024];
  stack_t ss;
  std::memset(&ss, 0, sizeof(ss));
  ss.ss_sp = alt_stack;
  ss.ss_size = sizeof(alt_stack);
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);

  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = OnFatalSignal;
  sigemptyset(&sa.sa_mask);
  // SA_ONSTACK: deliver on alt_stack. No SA_RESETHAND: stays installed across
  // firings (the guard must survive every case, not just the first).
  sa.sa_flags = SA_ONSTACK;
  for (int sig : {SIGABRT, SIGSEGV, SIGFPE, SIGBUS, SIGILL, SIGALRM}) {
    sigaction(sig, &sa, nullptr);
  }
}

inline const char* SignalName(int sig) {
  switch (sig) {
    case SIGABRT:
      return "SIGABRT (host-side check)";
    case SIGSEGV:
      return "SIGSEGV";
    case SIGFPE:
      return "SIGFPE";
    case SIGBUS:
      return "SIGBUS";
    case SIGILL:
      return "SIGILL";
    case SIGALRM:
      return "SIGALRM (timeout)";
    default:
      return "signal";
  }
}

// One-time startup equivalence assertion (tasks.md 4.2): a gaby Simulator
// seeded from the VIXL reference sim's ResetState snapshot reads back
// field-equivalent to that snapshot (sp/LR excluded — harness-owned). Returns
// true on success.
inline bool AssertEntryEquivalentOnce() {
  LiveEngines& eng = Engines();
  eng.ref_sim.ResetState();
  RegisterFile reference_reset{};
  for (unsigned i = 0; i < 31; ++i) {
    reference_reset.x[i] =
        static_cast<std::uint64_t>(eng.ref_sim.ReadXRegister(i));
  }
  for (unsigned i = 0; i < 32; ++i) {
    vixl::aarch64::SimVRegister& v = eng.ref_sim.ReadVRegister(i);
    reference_reset.v[i].lo = v.GetLane<std::uint64_t>(0);
    reference_reset.v[i].hi = v.GetLane<std::uint64_t>(1);
  }
  reference_reset.nzcv = eng.ref_sim.ReadNzcv().GetRawValue();
  reference_reset.fpcr = eng.ref_sim.ReadFpcr().GetRawValue();
  reference_reset.fpsr = 0;

  // Both gaby tracks are seeded identically (SeatEntry) before every body, so
  // both must read back equivalent to the reference ResetState — assert it for
  // each. Checking only cache_sim would leave a decoder-track seeding bug (e.g.
  // a field SeatEntry forgets to write on that sim) invisible until it produced
  // a silent divergence mid-body.
  SeatEntry(eng.cache_sim,
            reference_reset,
            eng.gaby_stack.data(),
            eng.gaby_stack.size());
  if (!EntrySeedingEquivalent(reference_reset,
                              eng.cache_sim.ReadAll(),
                              "startup (cache track)")) {
    return false;
  }
  SeatEntry(eng.decoder_sim,
            reference_reset,
            eng.gaby_stack.data(),
            eng.gaby_stack.size());
  return EntrySeedingEquivalent(reference_reset,
                                eng.decoder_sim.ReadAll(),
                                "startup (decoder track)");
}

// Expected ran/skipped counts per family — the coverage baseline. Without it
// the suite returns green whenever `failed == 0`, so a change that silently
// moves N cases from "ran" to "skipped" (a leaked sim knob like the old
// guarded-pages bug, a feature deny widening, a new abort that the crash guard
// skips) would shrink coverage invisibly. Pinning the counts turns any such
// drift into a hard failure. When a change legitimately shifts the counts (a
// fix revives skipped cases, a real new skip), re-run with
// VIXL_PORT_REBASELINE=1 to print the observed numbers, then update the table
// here in the same commit.
//
// The per-config fields exist for the case a debug-only host assertion shifts a
// body from "ran" to "skipped" under VIXL_DEBUG. Historically that happened for
// one integer case, branch_tagged_and_adr_adrp: the assembler's debug-only
// AllowPageOffsetDependentCode() assertion aborted its adrp-to-label assembly,
// which the crash guard caught as a skip. That is now fixed at the source —
// SETUP_CUSTOM honours the upstream PIC request (PageOffsetDependentCode), so
// the body assembles and runs under both configs (see gaby_two_track_macros.h).
// With it fixed, debug and release agree for every current family; the split
// fields are kept because the mechanism can recur after a VIXL upgrade.
struct FamilyBaseline {
  const char* family;
  int ran_debug;
  int skipped_debug;
  int ran_release;
  int skipped_release;
};

inline constexpr FamilyBaseline kFamilyBaselines[] = {
    {"integer", 193, 65, 193, 65},
    {"fp", 74, 2, 74, 2},
    {"neon", 254, 1, 254, 1},
    {"harness_smoke", 2, 0, 2, 0},  // G4 smoke: one scalar body + one RMW body
    // iOS runner: integer + fp + neon all link into one XCTest bundle (one
    // process), so a single RunRegisteredTests walk covers every registered
    // body. The baseline is the sum of the three families: ran 193+74+254=521,
    // skipped 65+2+1=68 — identical in debug and release now that the adrp
    // bodies assemble under both configs (see the struct comment above).
    {"ios_runner_all", 521, 68, 521, 68},
};

inline const FamilyBaseline* FindBaseline(const char* family) {
  for (const FamilyBaseline& b : kFamilyBaselines) {
    if (std::strcmp(b.family, family) == 0) {
      return &b;
    }
  }
  return nullptr;
}

// The coverage-baseline decision, factored out as a pure function so it has its
// own regression test (two_track_harness_selftest): a drift in either count is
// a violation unless an explicit rebaseline is in effect. Matching counts, or
// any counts under rebaseline, are not a violation.
inline bool IsBaselineViolation(int expected_ran,
                                int expected_skipped,
                                int ran,
                                int skipped,
                                bool rebaseline) {
  const bool drift = ran != expected_ran || skipped != expected_skipped;
  return drift && !rebaseline;
}

// Outcome of one family walk. The granular fields exist so the self-test can
// assert on the crash-guard and baseline behaviour precisely; `ran_ok` is the
// overall green/red verdict the int wrapper returns.
struct RunSummary {
  bool ran_ok = false;
  int total = 0;
  int ran = 0;
  int skipped = 0;
  int failed = 0;
  bool halted_on_gaby_crash = false;
  bool baseline_violation = false;
};

// Walk the registered TEST()s, run each under the guard + oracles, and tally
// into a RunSummary. (The thin int wrapper RunRegisteredTests() below is what
// family mains return from.)
inline RunSummary RunRegisteredTestsSummary(const char* family) {
  RunSummary s;
  if (!AssertEntryEquivalentOnce()) {
    std::fprintf(stderr,
                 "vixl_port[%s]: FATAL — gaby seeded entry state is not "
                 "equivalent to VIXL ResetState; oracle would be unsound.\n",
                 family);
    return s;  // ran_ok stays false
  }
  InstallGuards();

  const bool trace = std::getenv("VIXL_PORT_TRACE") != nullptr;
  // Triage switches (developer use, not CI):
  //   VIXL_PORT_ONLY=<substr>     run only TESTs whose name contains <substr>.
  //   VIXL_PORT_FRESH_ENGINES=1   rebuild the reference sim / cache / stack
  //                               before each case, to isolate a cross-case
  //                               state leak (e.g. a sticky sim knob).
  // Either one makes the run a non-standard subset, so the coverage baseline is
  // bypassed when triaging (see the gate below).
  const char* only = std::getenv("VIXL_PORT_ONLY");
  const bool fresh_engines = std::getenv("VIXL_PORT_FRESH_ENGINES") != nullptr;
  const bool triage = (only != nullptr) || fresh_engines;

  int total = 0, ran = 0, skipped = 0, failed = 0;
  // Set when a fatal signal fires inside a gaby track: we record the FAIL, then
  // stop the family — continuing is unsound (see the gaby-track branch below).
  bool gaby_track_crashed = false;

  for (vixl::Test* t = vixl::Test::first(); t != nullptr; t = t->next()) {
    if (only != nullptr && std::strstr(t->name(), only) == nullptr) {
      continue;  // filtered out — not counted, so the subset stays meaningful
    }
    ++total;
    ResetCurrent(t->name());
    if (trace) {
      std::fprintf(stderr, "[run] %s\n", t->name());
      std::fflush(stderr);
    }

    if (IsQuarantined(t->name())) {
      MarkSkip("quarantined by name (simulator-runtime/system body)");
    } else {
      if (fresh_engines) {
        RebuildEngines();  // pristine sim/cache/stack for this case
      }
      g_fatal_signal = 0;
      if (sigsetjmp(g_jmp, 1) == 0) {
        alarm(HostWatchdogSecondsForCurrentCase());
        t->run();  // SETUP/START/.../RUN/ASSERT fill Current()
        alarm(0);
      } else {
        // A fatal signal longjmped out of t->run(). WHERE it fired decides
        // whether this is a real failure or a legitimate skip (g_run_phase is
        // set tightly around each engine run; see RunPhase).
        alarm(0);
        const RunPhase phase = static_cast<RunPhase>(g_run_phase);
        const bool in_gaby_track =
            phase == RunPhase::kCacheTrack || phase == RunPhase::kDecoderTrack;
        if (in_gaby_track) {
          // A crash/hang INSIDE a gaby track (wild PC, out-of-range cache
          // entry, runaway loop) is the exact dispatch/predecode regression
          // class this guard rail exists to catch — a FAILURE, never a skip.
          Current().failed = true;
          const char* trk =
              phase == RunPhase::kCacheTrack ? "cache" : "decoder";
          std::fprintf(stderr,
                       "[FAIL] %s: %s while running the gaby %s track\n",
                       t->name(),
                       SignalName(g_fatal_signal),
                       trk);
          // The signal unwound past gaby's ExecutionScope destructor, so the
          // shared imported Simulator's `busy` re-entrancy flag stays latched
          // true: every later RunFrom/DebugRunFrom would be misread as a NESTED
          // call (cursor save/restore, no busy clear), silently corrupting all
          // subsequent cases. We cannot soundly continue on these engines and
          // the process-lifetime singletons are not cheaply rebuilt, so stop
          // the family. The FAIL already makes the suite red; fix and re-run.
          gaby_track_crashed = true;
        } else {
          // Assembly / reference-run abort: an unportable body, or a leaf the
          // reference sim itself rejects. Not a gaby divergence — skip.
          MarkSkip("aborted during run");
          std::fprintf(stderr,
                       "[skip] %s: %s\n",
                       t->name(),
                       SignalName(g_fatal_signal));
        }
      }
    }

    const CurrentCase& c = Current();
    // failed is checked BEFORE skipped: a body that records a real oracle
    // violation and then hits a late MarkSkip (e.g. a trailing
    // ASSERT_EQUAL_FP16 / SVE / NOT_EQUAL-consistency skip) is a FAILURE, not a
    // skip — the skip must never mask it.
    if (c.failed) {
      ++failed;
      std::fprintf(stderr, "[FAIL] %s\n", c.name.c_str());
    } else if (c.skipped) {
      ++skipped;
      if (trace) {
        std::fprintf(stderr,
                     "[skip] %s: %s\n",
                     c.name.c_str(),
                     c.skip_reason.c_str());
      }
    } else if (!c.ran) {
      // The body assembled but never reached RUN() (e.g. assembler-only veneer
      // / literal-pool-size tests, or trace/disasm bodies with no register
      // assertions). Not applicable to the two-track oracle — report as
      // skipped, not failed.
      ++skipped;
      if (trace) {
        std::fprintf(stderr,
                     "[skip] %s: body never invoked RUN()\n",
                     c.name.c_str());
      }
    } else {
      ++ran;
    }

    if (gaby_track_crashed) {
      // Stop the walk: the shared engines are now in an unsound state (latched
      // busy flag). `total` counts only cases actually reached, which is what
      // the baseline check below compares against — so a crash that halts the
      // family reads as a hard FAIL, not as a silently shorter run.
      s.halted_on_gaby_crash = true;
      std::fprintf(stderr,
                   "vixl_port[%s]: family halted after a gaby-track crash; "
                   "remaining cases not run\n",
                   family);
      break;
    }
  }

  s.total = total;
  s.ran = ran;
  s.skipped = skipped;
  s.failed = failed;
  std::printf(
      "vixl_port[%s]: %d TEST(s); %d passed (both tracks, both oracles), "
      "%d skipped, %d FAILED\n",
      family,
      total,
      ran,
      skipped,
      failed);
  if (total == 0) {
    if (only != nullptr) {
      std::fprintf(stderr,
                   "vixl_port[%s]: no TEST matched VIXL_PORT_ONLY=\"%s\"\n",
                   family,
                   only);
    } else {
      std::fprintf(stderr,
                   "vixl_port[%s]: ERROR — no TEST() registered\n",
                   family);
    }
    return s;  // ran_ok stays false
  }

  // In triage mode the run is a deliberately non-standard subset (filtered, or
  // fresh-per-case), so the coverage baseline does not apply — skip the gate
  // but still fail on any real oracle FAILURE, so triaging a crash still reads
  // red.
  if (triage) {
    std::fprintf(stderr,
                 "vixl_port[%s]: triage mode (VIXL_PORT_ONLY/FRESH_ENGINES) — "
                 "coverage baseline NOT enforced\n",
                 family);
    s.ran_ok = (failed == 0);
    return s;
  }

  // Coverage baseline gate (see kFamilyBaselines). Any drift in the ran/skipped
  // split is a hard failure unless an explicit rebaseline is requested. The
  // expected pair is config-specific (debug vs release — see kFamilyBaselines).
  const bool rebaseline = std::getenv("VIXL_PORT_REBASELINE") != nullptr;
  const FamilyBaseline* base = FindBaseline(family);
#ifdef NDEBUG
  const int expected_ran = base != nullptr ? base->ran_release : 0;
  const int expected_skipped = base != nullptr ? base->skipped_release : 0;
#else
  const int expected_ran = base != nullptr ? base->ran_debug : 0;
  const int expected_skipped = base != nullptr ? base->skipped_debug : 0;
#endif
  if (base == nullptr) {
    std::fprintf(stderr,
                 "vixl_port[%s]: ERROR — no coverage baseline for this "
                 "family (add one to kFamilyBaselines)\n",
                 family);
    s.baseline_violation = true;
  } else if (IsBaselineViolation(expected_ran,
                                 expected_skipped,
                                 ran,
                                 skipped,
                                 rebaseline)) {
    std::fprintf(stderr,
                 "vixl_port[%s]: FAIL — coverage drift: ran=%d skipped=%d, "
                 "expected ran=%d skipped=%d. A change moved cases between "
                 "ran and skipped. If intended, re-run with "
                 "VIXL_PORT_REBASELINE=1 and update kFamilyBaselines.\n",
                 family,
                 ran,
                 skipped,
                 expected_ran,
                 expected_skipped);
    s.baseline_violation = true;
  } else if (rebaseline &&
             (ran != expected_ran || skipped != expected_skipped)) {
    std::fprintf(stderr,
                 "vixl_port[%s]: REBASELINE — observed ran=%d skipped=%d "
                 "(table has ran=%d skipped=%d); update kFamilyBaselines\n",
                 family,
                 ran,
                 skipped,
                 expected_ran,
                 expected_skipped);
  }

  s.ran_ok = (failed == 0 && !s.baseline_violation);
  return s;
}

// Thin int wrapper: family mains return this. 0 iff the suite is green.
inline int RunRegisteredTests(const char* family) {
  return RunRegisteredTestsSummary(family).ran_ok ? 0 : 1;
}

}  // namespace vixl_port_live
}  // namespace gaby_vm

#endif  // GABY_VM_TEST_VIXL_ASM_GABY_TWO_TRACK_MAIN_H_
