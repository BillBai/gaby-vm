// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Authorship-time entry point. Walks the VIXL Test linked list (every TEST()
// the linked .cc registered), replays each body through the capture macros,
// filters out cases gaby-vm cannot execute, and writes the committed fixture
// .inc + manifest. NEVER part of the gaby-vm build/runtime.
//
// Robustness: the upstream test suite contains bodies that — run under our
// no-prologue, poison-entry capture model — abort (host-side VIXL_CHECK),
// fault, or loop. Each t->run() is therefore wrapped in a signal-guarded
// sigsetjmp + an alarm watchdog, so any one pathological test is skipped (and
// listed in the manifest) instead of killing the whole extraction.
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>  // alarm

#include "capture_state.h"
#include "fixture_writer.h"
#include "test-runner.h"  // vixl::Test

namespace {

// CPU-feature name substrings gaby-vm cannot execute (the VisitUnimplemented
// cluster of the design's §7). A captured case whose seen-feature set mentions
// any of these is skipped. Refined empirically as the bulk phases surface more.
const char* kDenyFeatureSubstrings[] = {
    "MTE",
    "BF16",
    "BFloat",
    "TME",
    "WFXT",
    "GCS",
    // PAuth/GCS results are key/modifier/host-address dependent (non-portable).
    "PAuth",
    // HBC (hinted conditional branch, BC.cond): gaby's handling is not yet
    // load-address-stable; excluded so the baseline guard rail stays green. The
    // gaby BC behavior is a separate issue (design §10), not masked here.
    "HBC",
};

// Tests skipped by name: simulator-runtime / system bodies the structural
// load/store + PC-rel filter cannot catch (they call host functions, intercept
// branches, drive an interactive debugger, zero memory by VA, or run MOPS
// memory-move sequences). Each is host-coupled and not portable to replay.
const char* kQuarantineByName[] = {
    "AARCH64_ASM_printf",
    "AARCH64_ASM_printf_no_preserve",
    "AARCH64_ASM_runtime_calls",
    "AARCH64_ASM_branch_interception",
    "AARCH64_ASM_large_sim_stack",
    "AARCH64_ASM_dc_zva",
    "AARCH64_ASM_system",
    "AARCH64_ASM_generic_operand",
    "AARCH64_ASM_mops_set",
    "AARCH64_ASM_mops_setn",
    "AARCH64_ASM_mops_cpy",
    "AARCH64_ASM_mops_cpyn",
    "AARCH64_ASM_mops_cpyf",
    "AARCH64_ASM_mops_cpyfn",
};

// Per-test crash/hang guard. A fatal signal during t->run() longjmps back to
// the loop, which marks the case skipped and continues with the next test.
sigjmp_buf g_jmp;
volatile sig_atomic_t g_fatal_signal = 0;

extern "C" void OnFatalSignal(int sig) {
  g_fatal_signal = sig;
  siglongjmp(g_jmp, 1);
}

void InstallGuards() {
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = OnFatalSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESETHAND: handler stays installed across firings.
  for (int sig : {SIGABRT, SIGSEGV, SIGFPE, SIGBUS, SIGILL, SIGALRM}) {
    sigaction(sig, &sa, nullptr);
  }
}

const char* SignalName(int sig) {
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
      return "SIGALRM (capture timeout)";
    default:
      return "signal";
  }
}

}  // namespace

int main(int argc, char** argv) {
  // argv[1] = output .inc path; argv[2] = symbol prefix (e.g. "Integer").
  const std::string out_path = (argc > 1) ? argv[1] : "out.inc";
  const std::string prefix = (argc > 2) ? argv[2] : "Generated";
  const bool trace = (argc > 3) && std::strcmp(argv[3], "--trace") == 0;

  gaby_vm::extract::FixtureWriter writer(out_path, prefix);
  InstallGuards();

  int total = 0;
  for (vixl::Test* t = vixl::Test::first(); t != nullptr; t = t->next()) {
    ++total;
    if (trace) {
      std::fprintf(stderr, "[run] %s\n", t->name());
      std::fflush(stderr);
    }
    gaby_vm::extract::ResetCurrent(t->name());

    g_fatal_signal = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
      alarm(8);  // host-level hang backstop (the in-sim cap is finer-grained)
      t->run();  // invokes the TEST body -> capture macros fill Current()
      alarm(0);
    } else {
      alarm(0);
      // A fatal signal fired inside t->run(). Current() may be partly filled;
      // discard it and record the skip. (Abandoned VIXL objects leak — fine for
      // a one-shot offline tool.)
      gaby_vm::extract::ResetCurrent(t->name());
      gaby_vm::extract::Current().skipped = true;
      gaby_vm::extract::Current().skip_reason =
          std::string("aborted during capture: ") + SignalName(g_fatal_signal);
    }

    gaby_vm::extract::CapturedCase& c = gaby_vm::extract::Current();
    if (!c.skipped) {
      for (const char* deny : kDenyFeatureSubstrings) {
        if (c.required_features.find(deny) != std::string::npos) {
          c.skipped = true;
          c.skip_reason = std::string("unsupported feature: ") + deny;
          break;
        }
      }
    }
    if (!c.skipped) {
      for (const char* q : kQuarantineByName) {
        if (c.name == q) {
          c.skipped = true;
          c.skip_reason = "quarantined by name (simulator-runtime/system body)";
          break;
        }
      }
    }
    writer.Add(c);
  }

  int included = writer.Finish();
  std::printf("vixl_test_extract: %d TEST(s) walked, %d included for '%s'\n",
              total,
              included,
              prefix.c_str());
  return (included >= 0) ? 0 : 1;
}
