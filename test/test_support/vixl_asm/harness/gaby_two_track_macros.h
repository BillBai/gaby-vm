// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Redefines VIXL's test macros so an upstream VIXL TEST() body, compiled
// verbatim, ASSEMBLES LIVE at test time and runs on BOTH gaby tracks under an
// absolute oracle (the VIXL reference simulator's body-exit register file AND
// the body's exit memory image, read directly — not via core.Dump) and a
// differential oracle (cache track == decoder track, registers AND memory).
//
// This is the live-assemble successor to
// tools/vixl_test_extract/capture_macros.h: same macro-redefinition trick (the
// build feeds a copy of the upstream test .cc with its no-guard `#include
// "test-assembler-aarch64.h"` stripped, and this header — included first —
// supplies the macros), but RUN() now executes the two gaby tracks immediately
// instead of capturing frozen bytes. Baked host addresses (Mov(reg,
// reinterpret_cast<uintptr_t>(buf))) now point at valid in-process memory, so
// load/store/ADR/literal bodies run for real.
//
// no-JIT / no-RWX: assembled bytes are ordinary malloc data fed to the gaby
// decoder. Nothing is natively executed (the island forces
// VIXL_CODE_BUFFER_MALLOC and never copies test-utils.cc's ExecuteMemory mmap
// path).
#ifndef GABY_VM_TEST_VIXL_ASM_GABY_TWO_TRACK_MACROS_H_
#define GABY_VM_TEST_VIXL_ASM_GABY_TWO_TRACK_MACROS_H_

#include <algorithm>
#include <cinttypes>
#include <csignal>  // sig_atomic_t (g_run_phase)
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "code-generation-scopes-vixl.h"  // IWYU pragma: keep
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"
#include "vixl_port_oracle.h"  // gaby-side oracle + SeatEntry (gaby public API)

#include "aarch64/cpu-features-auditor-aarch64.h"  // IWYU pragma: keep
#include "aarch64/macro-assembler-aarch64.h"
#include "aarch64/simulator-aarch64.h"
#include "aarch64/test-utils-aarch64.h"  // IWYU pragma: keep

namespace gaby_vm {
namespace vixl_port_live {

// `br xzr` raw encoding — branch to register 31 (zero register) = address 0,
// the gaby end-of-simulation sentinel. Appended to a body so the run terminates
// without depending on LR. Emitted as a raw word (MacroAssembler::Br(xzr)
// asserts !IsZero() under VIXL_DEBUG).
inline constexpr std::uint32_t kBrXzr = 0xd61f03e0u;

// Caps a single body's reference-sim run. A body that loops past this (e.g. a
// far-branch padding test, or a count register left at the ResetState poison)
// is skipped *before* the gaby tracks run it — so the reference cap doubles as
// the per-case instruction cap protecting the unbounded gaby RunFrom.
// Matches the extraction tool's proven cap. A body that runs past this is
// either looping on a count register left at the ResetState poison (there is no
// VIXL prologue to initialise it) or a far-branch-range padding test — neither
// is a useful leaf guard rail. Kept low so such a body hits the cap and skips
// in well under a second (a higher cap would run a debug sim for tens of
// seconds and trip the host watchdog instead, with a less descriptive skip
// reason).
inline constexpr std::uint64_t kMaxInstructions = 8'192;
// One upstream literal-range case intentionally executes roughly two AArch64
// literal-load ranges of NOP padding to prove the assembler emits pools before
// they go out of reach. Now that embedded data no longer rejects registration,
// that case is a useful data-in-stream guard rail, so it gets a targeted cap.
inline constexpr std::uint64_t kLongPaddingMaxInstructions = 600'000;

// Per-case result, filled by the macros and read by the family main().
struct CurrentCase {
  std::string name = "?";
  bool skipped = false;
  std::string skip_reason;
  bool ran = false;
  bool failed = false;  // any absolute/differential oracle violation
};

inline CurrentCase& Current() {
  static CurrentCase c;
  return c;
}

inline bool IsLongPaddingCase() {
  return Current().name == "AARCH64_ASM_ldr_literal_range";
}

inline std::uint64_t MaxInstructionsForCurrentCase() {
  return IsLongPaddingCase() ? kLongPaddingMaxInstructions : kMaxInstructions;
}

// Which engine TwoTrackRun is currently driving. Read by the family main's
// crash/hang guard the moment a fatal signal longjmps out of t->run(): a signal
// that fires while a GABY track (cache or decoder) is executing is a genuine
// gaby execution failure — a wild PC, an out-of-range cache entry, a runaway
// loop — exactly the dispatch/predecode regression class this guard rail exists
// to catch, so it must FAIL the suite. A signal during assembly or the
// reference run instead means an unportable body or a leaf the reference sim
// itself rejects, which stays a skip. Set tightly around each engine call and
// reset to kOutsideRun immediately after, so only the actual execution window
// is attributed. `volatile sig_atomic_t` because it is read just after
// returning from the signal handler via siglongjmp.
enum class RunPhase : int {
  kOutsideRun = 0,  // assembly, oracle comparison, or between cases
  kReference,       // VIXL reference sim
  kCacheTrack,      // gaby cache track (RunFrom)
  kDecoderTrack,    // gaby decoder track (DebugRunFrom)
};

inline volatile sig_atomic_t g_run_phase =
    static_cast<int>(RunPhase::kOutsideRun);

inline void SetRunPhase(RunPhase p) { g_run_phase = static_cast<int>(p); }

// Test-only fault-injection seam. The gaby-track crash guard (a fatal signal
// while a gaby track runs -> FAIL + halt the family) and its negative (a signal
// during the reference run -> skip) have no naturally-occurring trigger in the
// suite — they are armed for the day an aggressive cache/dispatch change goes
// wild. So `two_track_harness_selftest` injects a fault at a chosen phase and
// asserts the harness classifies it correctly. Compiled in ONLY when the
// self-test TU defines GABY_VM_VIXL_PORT_SELFTEST; in the real family binaries
// it is an empty inline that optimizes away entirely (no production change).
#ifdef GABY_VM_VIXL_PORT_SELFTEST
inline void (*g_selftest_phase_fault)(RunPhase) = nullptr;
inline void MaybeInjectPhaseFault(RunPhase p) {
  if (g_selftest_phase_fault != nullptr) {
    g_selftest_phase_fault(p);
  }
}
#else
inline void MaybeInjectPhaseFault(RunPhase) {}
#endif

inline void ResetCurrent(const char* name) {
  CurrentCase& c = Current();
  c.name = name;
  c.skipped = false;
  c.skip_reason.clear();
  c.ran = false;
  c.failed = false;
  SetRunPhase(RunPhase::kOutsideRun);
}

inline void MarkSkip(const char* reason) {
  if (Current().skipped) {
    return;  // first reason wins
  }
  Current().skipped = true;
  Current().skip_reason = reason;
}

// Guest-visible output sink for the (rare, quarantined) Printf runtime call, so
// a stray guest print does not spam the test log.
inline std::FILE* NullStream() {
  static std::FILE* f = std::fopen("/dev/null", "w");
  return (f != nullptr) ? f : stderr;
}

// Process-lifetime engines, constructed once and reused across every TEST in
// the family (constructing a debug Simulator is ~100ms; amortising it keeps the
// family's wall-clock in the sub-second range, like the frozen runner did).
struct LiveEngines {
  // VIXL reference: computes the absolute-oracle anchor (core.Dump).
  vixl::aarch64::Decoder ref_decoder;
  vixl::aarch64::Simulator ref_sim;

  // gaby tracks share one PredecodeCache and one stack; both are re-seeded per
  // case. The cache is append-only, so each case's body slice is registered as
  // it comes and the slice buffers are kept alive here for the cache's
  // lifetime.
  gaby_vm::PredecodeCache cache;
  alignas(16) std::array<std::uint8_t, 64 * 1024> gaby_stack{};
  gaby_vm::Simulator cache_sim;
  gaby_vm::Simulator decoder_sim;
  std::deque<std::vector<std::uint32_t>> kept_buffers;

  LiveEngines()
      : ref_sim(&ref_decoder, NullStream()),
        cache_sim(&cache, gaby_stack.data(), gaby_stack.size()),
        decoder_sim(nullptr, gaby_stack.data(), gaby_stack.size()) {
    // gaby registers leaves under CPUFeatures::All(); mirror that so no body is
    // gated out at assemble/run time (we filter by *seen* features instead).
    ref_sim.SetCPUFeatures(vixl::CPUFeatures::All());
  }
};

// Held behind a unique_ptr so the VIXL_PORT_FRESH_ENGINES triage switch can
// reconstruct the engines between cases (to isolate a cross-case state leak).
// In the normal path it is constructed once, lazily, and Engines() returns the
// same reference as before — no behaviour change.
inline std::unique_ptr<LiveEngines>& EnginesHolder() {
  static std::unique_ptr<LiveEngines> e = std::make_unique<LiveEngines>();
  return e;
}

inline LiveEngines& Engines() { return *EnginesHolder(); }

// Triage only (VIXL_PORT_FRESH_ENGINES): drop the current engines and build a
// fresh set, so the next case starts from a pristine reference sim / cache /
// stack. Never called on the normal CI path.
inline void RebuildEngines() {
  EnginesHolder() = std::make_unique<LiveEngines>();
}

// A RegisterDump-shaped view over a gaby RegisterFile. The absolute-oracle
// anchor is the reference sim's post-run register state, read directly into a
// RegisterFile (`ref_state`) — NOT VIXL's core.Dump. core.Dump would require
// running the reference on a buffer that includes the emitted dump code, at a
// different address than the gaby tracks run their slice, so PC-relative
// results (ADR/ADRP/LDR-literal) would diverge between the reference and gaby.
// Instead all three engines run the SAME slice at the SAME address and we read
// the reference's registers directly. This view supplies the few `core.xreg /
// sreg_bits / dreg_bits` calls some upstream bodies make in assert expressions.
struct RefStateView {
  const gaby_vm::RegisterFile* rf = nullptr;
  std::uint64_t xreg(unsigned code) const {
    return (code == 31) ? rf->sp : rf->x[code];
  }
  std::uint32_t wreg(unsigned code) const {
    return static_cast<std::uint32_t>(xreg(code) & 0xffffffffu);
  }
  std::uint32_t sreg_bits(unsigned code) const {
    return static_cast<std::uint32_t>(rf->v[code].lo & 0xffffffffu);
  }
  std::uint16_t hreg_bits(unsigned code) const {
    return static_cast<std::uint16_t>(rf->v[code].lo & 0xffffu);
  }
  std::uint64_t dreg_bits(unsigned code) const { return rf->v[code].lo; }
  std::uint32_t flags_nzcv() const { return rf->nzcv; }
};

// The three register-file snapshots TwoTrackRun fills DURING the engine runs
// (after the frame snapshot is taken) MUST live outside the body's stack frame
// — the harness resets that frame between runs to reset the body's RMW locals,
// and would otherwise wipe the oracle's just-captured state. They are held
// here, in process-lifetime storage, and reached through reference members of
// LiveRig so every `rig.ref_state` / `rig.cache_state` / `rig.decoder_state`
// use is unchanged. (The rig itself stays a per-case SETUP() local — a fresh
// MacroAssembler per case, so a body that aborts mid-assembly under the crash
// guard cannot leak a blocked-literal-pool state into the next case.)
struct LiveState {
  gaby_vm::RegisterFile
      ref_state{};  // reference sim's body-exit state (anchor)
  gaby_vm::RegisterFile cache_state{};
  gaby_vm::RegisterFile decoder_state{};
};

inline LiveState& State() {
  static LiveState s;
  return s;
}

// Per-case state: a fresh MacroAssembler (cheap) plus references into the
// reused engines, the process-lifetime register-file snapshots, and the
// absolute-oracle anchor (`ref_state`). Created by SETUP() as a stack local.
struct LiveRig {
  vixl::aarch64::MacroAssembler masm;
  LiveEngines& eng = Engines();
  std::ptrdiff_t body_start = 0;
  std::ptrdiff_t body_end = 0;
  gaby_vm::RegisterFile entry{};  // unified entry; set before the runs
  // Backed by State() (outside this frame) so the frame reset can't wipe them.
  gaby_vm::RegisterFile& ref_state = State().ref_state;
  gaby_vm::RegisterFile& cache_state = State().cache_state;
  gaby_vm::RegisterFile& decoder_state = State().decoder_state;
  RefStateView core{
      &State().ref_state};  // `core` alias for upstream body exprs
};

// Snapshot the reference sim's post-ResetState architectural state into a gaby
// RegisterFile. sp and x30/LR are filled too but SeatEntry overrides them (the
// harness owns sp/LR). Called from START(), before any execution.
inline void SnapshotEntry(LiveRig& rig) {
  vixl::aarch64::Simulator& s = rig.eng.ref_sim;
  for (unsigned i = 0; i < 31; ++i) {
    rig.entry.x[i] = static_cast<std::uint64_t>(s.ReadXRegister(i));
  }
  for (unsigned i = 0; i < 32; ++i) {
    vixl::aarch64::SimVRegister& v = s.ReadVRegister(i);
    rig.entry.v[i].lo = v.GetLane<std::uint64_t>(0);
    rig.entry.v[i].hi = v.GetLane<std::uint64_t>(1);
  }
  rig.entry.nzcv = s.ReadNzcv().GetRawValue();
  rig.entry.fpcr = s.ReadFpcr().GetRawValue();
  rig.entry.fpsr = 0;  // the imported simulator does not model FPSR.
  rig.entry.sp = 0;    // overridden by SeatEntry.
  rig.entry.pc = 0;
  rig.entry.btype = 0;
}

// Run the body slice on the reference sim (the SAME buffer + address the gaby
// tracks use, so PC-relative results agree), bounded by kMaxInstructions.
// Returns false (and marks the case skipped) on the cap.
inline bool RunReferenceOnSlice(LiveRig& rig, std::uintptr_t entry_addr) {
  using vixl::aarch64::Instruction;
  vixl::aarch64::Simulator& s = rig.eng.ref_sim;
  s.WritePc(reinterpret_cast<const Instruction*>(entry_addr));
  std::uint64_t executed = 0;
  const std::uint64_t max_instructions = MaxInstructionsForCurrentCase();
  while (!s.IsSimulationFinished()) {
    if (executed++ >= max_instructions) {
      MarkSkip("instruction cap exceeded (looping or far-branch-padding body)");
      return false;
    }
    s.ExecuteInstruction();
  }
  return true;
}

// Snapshot the reference sim's body-exit architectural state into ref_state —
// the absolute-oracle anchor (replaces VIXL's core.Dump).
inline void SnapshotRef(LiveRig& rig) {
  vixl::aarch64::Simulator& s = rig.eng.ref_sim;
  for (unsigned i = 0; i < 31; ++i) {
    rig.ref_state.x[i] = static_cast<std::uint64_t>(s.ReadXRegister(i));
  }
  rig.ref_state.sp = static_cast<std::uint64_t>(
      s.ReadXRegister(31, vixl::aarch64::Reg31IsStackPointer));
  for (unsigned i = 0; i < 32; ++i) {
    vixl::aarch64::SimVRegister& v = s.ReadVRegister(i);
    rig.ref_state.v[i].lo = v.GetLane<std::uint64_t>(0);
    rig.ref_state.v[i].hi = v.GetLane<std::uint64_t>(1);
  }
  rig.ref_state.nzcv = s.ReadNzcv().GetRawValue();
  rig.ref_state.fpcr = s.ReadFpcr().GetRawValue();
  rig.ref_state.fpsr = 0;
}

// Slice the body bytes [body_start, body_end) out of the masm buffer, append
// the raw br xzr terminator, keep the buffer alive, and register it with the
// cache. Returns the entry address. A registration failure is reported as a
// SKIP (not a failure): it means the cache cannot represent this body —
// typically an inline literal pool whose constant bytes decode to an
// unsupported-feature "instruction" when the cache predecodes the whole range.
// That is a cache limitation for data-in-stream, not a gaby correctness
// divergence.
inline std::uintptr_t BuildAndRegisterGabySlice(LiveRig& rig) {
  const auto* base =
      rig.masm.GetBuffer()->GetStartAddress<const std::uint32_t*>();
  std::size_t first =
      static_cast<std::size_t>(rig.body_start) / sizeof(std::uint32_t);
  std::size_t last =
      static_cast<std::size_t>(rig.body_end) / sizeof(std::uint32_t);
  rig.eng.kept_buffers.emplace_back(base + first, base + last);
  std::vector<std::uint32_t>& prog = rig.eng.kept_buffers.back();
  prog.push_back(kBrXzr);
  if (rig.eng.cache.RegisterCodeRange(prog.data(),
                                      prog.size() * sizeof(std::uint32_t)) !=
      gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
    MarkSkip("cache RegisterCodeRange rejected the slice (data-in-stream)");
  }
  return reinterpret_cast<std::uintptr_t>(prog.data());
}

// Zero the shared guest stack so each engine runs from identical stack memory.
// All three engines run with sp pointing into gaby_stack (the reference's sp is
// re-pointed there in START), and they run sequentially, so without this a
// later engine could read a value an earlier one left below sp.
inline void ResetGabyStack(LiveRig& rig) {
  std::memset(rig.eng.gaby_stack.data(), 0, rig.eng.gaby_stack.size());
}

// Apply the feature deny-list against the reference sim's seen-feature set. A
// body needing a leaf gaby does not model is skipped (not failed).
const char* const kDenyFeatureSubstrings[] = {
    "MTE",
    "BF16",
    "BFloat",
    "TME",
    "WFXT",
    "GCS",
    "PAuth",
    "HBC",
};

inline void ApplyFeatureDenyList(LiveRig& rig) {
  std::ostringstream os;
  os << rig.eng.ref_sim.GetSeenFeatures();
  const std::string seen = os.str();
  for (const char* deny : kDenyFeatureSubstrings) {
    if (seen.find(deny) != std::string::npos) {
      MarkSkip("unsupported feature (deny-list)");
      return;
    }
  }
}

// Per-case reset of the memory the three engines share. A body bakes the host
// addresses of its own C-stack locals (e.g. `uint64_t data[2]`) into the
// instruction stream, so all three engines load/store the SAME locals.
// Resetting them between runs is what lets read-modify-write bodies (atomics /
// exclusives / CAS / NEON store-multiple) run on all three: each engine starts
// from identical memory, so an earlier engine's store cannot leak into a later
// engine's load.
//
// The window is the body function's whole stack frame: [tt_fp, body_fp), where
// body_fp is the body's frame pointer (captured in the RUN() macro, which
// inlines into the body) and tt_fp is TwoTrackRun's frame pointer. The body's
// locals live in that range; TwoTrackRun's own frame (and the `save` buffer's
// owning vector) sit BELOW tt_fp, outside the window, so restoring it cannot
// corrupt the harness. During each engine run the C++ frames above tt_fp are
// dormant (control is in TwoTrackRun's callees, below tt_fp), so the only thing
// that changes in the window is the guest's stores to the body's locals —
// exactly what we reset. `gaby_stack` (the guest stack, a separate buffer) is
// reset alongside.
struct FrameWindow {
  std::uint8_t* lo = nullptr;
  std::uint8_t* hi = nullptr;
  std::vector<std::uint8_t> save;
  bool active() const { return lo != nullptr && hi > lo; }
};

inline FrameWindow SnapshotFrame(std::uint8_t* tt_fp, void* body_frame) {
  FrameWindow w;
  auto* body_fp = static_cast<std::uint8_t*>(body_frame);
  // Sanity: body frame must be above ours and of a sane size (frames are KB-
  // scale; a huge span means we mis-read the frame pointers — fall back to no
  // windowing rather than touch unrelated memory).
  constexpr std::size_t kMaxFrame = 256 * 1024;
  if (body_fp > tt_fp &&
      static_cast<std::size_t>(body_fp - tt_fp) <= kMaxFrame) {
    w.lo = tt_fp;
    w.hi = body_fp;
    w.save.assign(w.lo, w.hi);
  }
  return w;
}

inline void RestoreFrame(const FrameWindow& w) {
  if (w.active()) {
    std::memcpy(w.lo, w.save.data(), w.save.size());
  }
}

// Snapshot the current window bytes (the body's locals, holding whichever
// engine just ran). Returns empty if the window is inactive — then the memory
// oracle is skipped, exactly as the frame reset is. The vector lives in
// TwoTrackRun's own frame (below the window), so capturing it does not perturb
// the window.
inline std::vector<std::uint8_t> CaptureWindow(const FrameWindow& w) {
  if (w.active()) {
    return std::vector<std::uint8_t>(w.lo, w.hi);
  }
  return {};
}

// Compare two window snapshots byte-for-byte. On the first mismatch print a
// [FAIL] line locating the case, the oracle, and the differing 8-byte-aligned
// qword from each snapshot (usually the wrong stored value), then return false.
// The non-guest bytes of the window (rig_, padding) are identical across runs —
// only the guest's stores to the body's locals can differ — so a mismatch is a
// genuine memory side-effect divergence.
inline bool WindowMemoryEqual(const char* name,
                              const char* oracle,
                              const char* lhs_label,
                              const std::vector<std::uint8_t>& lhs,
                              const char* rhs_label,
                              const std::vector<std::uint8_t>& rhs) {
  const std::size_t n = std::min(lhs.size(), rhs.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (lhs[i] == rhs[i]) {
      continue;
    }
    const std::size_t q = i & ~std::size_t{7};
    auto qword = [&](const std::vector<std::uint8_t>& v) {
      std::uint64_t x = 0;
      for (std::size_t b = 0; b < 8 && q + b < v.size(); ++b) {
        x |= static_cast<std::uint64_t>(v[q + b]) << (8 * b);
      }
      return x;
    };
    std::fprintf(stderr,
                 "[FAIL] %s / %s (%s vs %s): guest memory differs at frame "
                 "offset %zu\n"
                 "  %-7s = 0x%016" PRIx64
                 "\n"
                 "  %-7s = 0x%016" PRIx64 "\n",
                 name,
                 oracle,
                 lhs_label,
                 rhs_label,
                 q,
                 lhs_label,
                 qword(lhs),
                 rhs_label,
                 qword(rhs));
    return false;
  }
  return true;
}

// The whole RUN(): build the body slice, run all three engines on it (same
// buffer, same address, identical seeded entry, and identical reset memory —
// the guest stack AND the body's own locals), then the register differential
// oracle and the memory oracle. The reference's body-exit registers (ref_state)
// are the absolute-oracle anchor for the body's ASSERT_EQUAL_* expansions; its
// body-exit memory image is the anchor for the memory oracle here. Each
// engine's store image is captured from the frame window the moment that engine
// finishes, before the next reset — the register snapshots do NOT cover memory
// side effects (STR / STP / atomics / CAS / NEON store-multiple), so a store
// the shared leaf gets wrong on BOTH tracks would otherwise pass undetected.
// `noinline` so its frame pointer is genuinely below the body's (the frame
// window relies on it).
__attribute__((noinline)) inline void TwoTrackRun(LiveRig& rig,
                                                  void* body_frame) {
  if (Current().skipped) {
    return;
  }
  // Build + register the slice first, so the reference runs the SAME bytes at
  // the SAME address as the gaby tracks (PC-relative results — ADR/ADRP/literal
  // loads — would otherwise diverge).
  const std::uintptr_t entry_addr = BuildAndRegisterGabySlice(rig);
  if (Current().skipped) {
    return;  // cache rejected the slice
  }
  void* stk = rig.eng.gaby_stack.data();
  std::size_t stk_sz = rig.eng.gaby_stack.size();

  // Snapshot the body's locals (at their initial, post-C++-init values — the
  // body assembled but has not executed yet) so each engine runs from identical
  // memory.
  FrameWindow window =
      SnapshotFrame(static_cast<std::uint8_t*>(__builtin_frame_address(0)),
                    body_frame);
  // An inactive window silently disables BOTH the per-run memory reset AND the
  // memory oracle for this body — a real coverage hole if it ever happens to an
  // RMW body. It should not occur for normal bodies (only a >256KB frame or an
  // unreadable frame pointer trips it), so make it loud rather than silent:
  // warn once, naming the first offender, regardless of VIXL_PORT_TRACE.
  if (!window.active()) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      std::fprintf(stderr,
                   "[warn] %s: frame window inactive (frame too large or "
                   "unreadable) — memory reset AND memory oracle are disabled "
                   "for this and any further such bodies\n",
                   Current().name.c_str());
    }
  }

  // Reference run -> ref_state. The reference's sp/LR were unified to the gaby
  // stack in START, so it runs against the same guest stack the gaby tracks
  // use.
  ResetGabyStack(rig);
  SetRunPhase(RunPhase::kReference);
  MaybeInjectPhaseFault(
      RunPhase::kReference);  // test-only; no-op in production
  const bool ref_ok = RunReferenceOnSlice(rig, entry_addr);
  SetRunPhase(RunPhase::kOutsideRun);
  if (!ref_ok) {
    RestoreFrame(window);  // leave the body's locals as the body's C++ expects
    return;                // cap exceeded -> skipped
  }
  ApplyFeatureDenyList(rig);
  if (Current().skipped) {
    RestoreFrame(window);
    return;
  }
  SnapshotRef(rig);
  // Capture the reference's store image WHILE the window still holds it (before
  // the reset below) — the absolute-oracle anchor for memory.
  const std::vector<std::uint8_t> ref_mem = CaptureWindow(window);

  RestoreFrame(window);
  ResetGabyStack(rig);
  SeatEntry(rig.eng.cache_sim, rig.entry, stk, stk_sz);
  // The two gaby RunFrom calls are the only windows in which a fatal signal is
  // attributed to a gaby track (and thus FAILs the suite — see RunPhase). The
  // reset to kOutsideRun is immediate so the surrounding harness reads/captures
  // are never mis-attributed.
  SetRunPhase(RunPhase::kCacheTrack);
  MaybeInjectPhaseFault(
      RunPhase::kCacheTrack);  // test-only; no-op in production
  rig.eng.cache_sim.RunFrom(entry_addr);
  SetRunPhase(RunPhase::kOutsideRun);
  rig.cache_state = rig.eng.cache_sim.ReadAll();
  const std::vector<std::uint8_t> cache_mem = CaptureWindow(window);

  RestoreFrame(window);
  ResetGabyStack(rig);
  SeatEntry(rig.eng.decoder_sim, rig.entry, stk, stk_sz);
  SetRunPhase(RunPhase::kDecoderTrack);
  MaybeInjectPhaseFault(
      RunPhase::kDecoderTrack);  // test-only; no-op in production
  rig.eng.decoder_sim.DebugRunFrom(entry_addr);
  SetRunPhase(RunPhase::kOutsideRun);
  rig.decoder_state = rig.eng.decoder_sim.ReadAll();
  const std::vector<std::uint8_t> decoder_mem = CaptureWindow(window);

  const char* name = Current().name.c_str();
  if (!DifferentialEqual(rig.cache_state, rig.decoder_state, name)) {
    Current().failed = true;
  }
  // Memory oracle: differential (the two gaby tracks must store identically)
  // then absolute (gaby's stores must match the reference's). cache == decoder
  // and cache == ref together imply all three agree. Skipped when the window is
  // inactive (a >256KB body frame or unreadable frame pointers), exactly as the
  // frame reset is.
  if (window.active()) {
    if (!WindowMemoryEqual(name,
                           "memory differential",
                           "cache",
                           cache_mem,
                           "decoder",
                           decoder_mem)) {
      Current().failed = true;
    } else if (!WindowMemoryEqual(name,
                                  "memory absolute",
                                  "gaby",
                                  cache_mem,
                                  "ref",
                                  ref_mem)) {
      Current().failed = true;
    }
  }
  Current().ran = true;
}

// --- ASSERT_EQUAL_* duck typing (from capture_macros.h) --------------------
// Upstream asserts admit register, scalar, and exotic operand shapes; classify
// by duck typing so the whole .cc compiles while only the checkable shapes turn
// into oracle comparisons.

template <class T, class = void>
struct HasGetCode : std::false_type {};
template <class T>
struct HasGetCode<T, std::void_t<decltype(std::declval<const T&>().GetCode())>>
    : std::true_type {};

// Both vixl Register and VRegister expose IsVRegister(); integer immediates do
// not. We use it to read the right register BANK: an X register and a V
// register can share a code (x17 vs d17) yet live in different files, so
// reading v.GetCode() into the X array (as the old duck typing did) silently
// mis-read e.g. ASSERT_EQUAL_64(<bits>, d17) against x17. That made the
// reference disagree with the upstream literal and wrongly skipped the body.
template <class T, class = void>
struct HasIsVRegister : std::false_type {};
template <class T>
struct HasIsVRegister<
    T,
    std::void_t<decltype(std::declval<const T&>().IsVRegister())>>
    : std::true_type {};

// Classified register operand: whether the operand is a register at all, which
// bank it belongs to, and its code. A non-register (integer immediate / exotic
// expression) yields is_reg == false.
struct RegOperand {
  bool is_reg = false;
  bool is_vector = false;  // V/SIMD register (read the v[] bank), else X bank
  unsigned code = 0;
};

template <class T>
RegOperand ClassifyReg(const T& r) {
  RegOperand o;
  if constexpr (HasGetCode<T>::value) {
    o.is_reg = true;
    o.code = r.GetCode();
    if constexpr (HasIsVRegister<T>::value) {
      o.is_vector = r.IsVRegister();
    }
  }
  return o;
}

// Reference value for a 64-bit comparison against a register operand: the V
// bank's low 64-bit lane for a vector register, else the X (or sp) value.
inline std::uint64_t RefValue64(const RegOperand& op,
                                const RefStateView& core) {
  return op.is_vector ? core.dreg_bits(op.code)
                      : static_cast<std::uint64_t>(core.xreg(op.code));
}

template <class T>
std::uint64_t ResolveExpected(const T& v, const RefStateView& core, bool& ok) {
  if constexpr (HasGetCode<T>::value) {
    ok = true;
    return RefValue64(ClassifyReg(v), core);
  } else if constexpr (std::is_convertible_v<T, std::uint64_t>) {
    ok = true;
    return static_cast<std::uint64_t>(v);
  } else {
    ok = false;
    return 0;
  }
}

template <class T>
bool ResultRegCode(const T& r, unsigned& code) {
  if constexpr (HasGetCode<T>::value) {
    code = r.GetCode();
    return true;
  } else {
    return false;
  }
}

// Both gaby tracks must match the reference value `ref_val` for register
// `code`.
template <class CheckFn>
void CheckBothTracks(LiveRig& rig, CheckFn check) {
  if (!check(rig.cache_state, "cache")) {
    Current().failed = true;
  }
  if (!check(rig.decoder_state, "decoder")) {
    Current().failed = true;
  }
}

// Pin both gaby tracks' register `op` to the reference value `want`, reading
// the correct bank (V vs X). A vector operand compares the V file's low lane
// (CheckFP64 / CheckFP32 read v[code]); an integer operand the X (or sp) file.
inline void PinReg64(LiveRig& rig,
                     const RegOperand& op,
                     std::uint64_t want,
                     const char* name) {
  CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
    return op.is_vector ? CheckFP64(s, op.code, want, trk, name)
                        : CheckX(s, op.code, want, trk, name);
  });
}

inline void PinReg32(LiveRig& rig,
                     const RegOperand& op,
                     std::uint64_t want,
                     const char* name) {
  const std::uint32_t want32 = static_cast<std::uint32_t>(want & 0xffffffffu);
  CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
    return op.is_vector ? CheckFP32(s, op.code, want32, trk, name)
                        : CheckW(s, op.code, want, trk, name);
  });
}

template <class Exp, class Res>
void AssertEqual64(LiveRig& rig, const Exp& expected, const Res& result) {
  if (Current().skipped || !Current().ran) {
    return;
  }
  bool exp_ok = false;
  std::uint64_t want = ResolveExpected(expected, rig.core, exp_ok);
  const RegOperand res = ClassifyReg(result);
  if (!exp_ok || !res.is_reg) {
    // Either the expected operand is exotic (not a register or integer), or the
    // result is a MEMORY value (e.g. ASSERT_EQUAL_64(v, dst[0])) rather than a
    // register. A memory result is not dropped from coverage: the body's store
    // image is validated wholesale by the frame-window memory oracle in
    // TwoTrackRun (both tracks vs each other and vs the reference), which is
    // strictly stronger than this per-assert form and avoids the type ambiguity
    // between a memory read-back and an immediate. So this drop loses nothing.
    return;
  }
  // Read the result's reference value from its own bank:
  // ASSERT_EQUAL_64(<bits>, dN) is a V-register compare (low 64-bit lane), not
  // the same-numbered X.
  std::uint64_t ref_val = RefValue64(res, rig.core);
  if (ref_val != want) {
    MarkSkip(
        "reference sim disagrees with the upstream ASSERT_EQUAL_64 literal");
    return;
  }
  const char* name = Current().name.c_str();
  PinReg64(rig, res, ref_val, name);
  // When `expected` is itself a register (e.g. ASSERT_EQUAL_64(x4, x5)), pin it
  // to the reference too. Pinning only `result` would miss the case the
  // upstream "expected == result" is meant to catch: both gaby tracks
  // mis-compute the expected register while `result` stays correct — the
  // absolute oracle on `result` alone still passes, but gaby's expected !=
  // gaby's result, breaking the asserted equality. `want` already holds the
  // reference's value for that register (ResolveExpected reads the right bank).
  // An immediate expected has no register to pin.
  const RegOperand exp = ClassifyReg(expected);
  if (exp.is_reg) {
    PinReg64(rig, exp, want, name);
  }
}

template <class Exp, class Res>
void AssertEqual32(LiveRig& rig, const Exp& expected, const Res& result) {
  if (Current().skipped || !Current().ran) {
    return;
  }
  bool exp_ok = false;
  std::uint64_t want =
      ResolveExpected(expected, rig.core, exp_ok) & 0xffffffffu;
  const RegOperand res = ClassifyReg(result);
  if (!exp_ok || !res.is_reg) {
    return;  // exotic expected or a memory result — see AssertEqual64; a memory
             // result is covered wholesale by the frame-window memory oracle.
  }
  std::uint64_t ref_val = RefValue64(res, rig.core) & 0xffffffffu;
  if (ref_val != want) {
    MarkSkip(
        "reference sim disagrees with the upstream ASSERT_EQUAL_32 literal");
    return;
  }
  const char* name = Current().name.c_str();
  PinReg32(rig, res, ref_val, name);
  // Pin a register `expected` too (see AssertEqual64); `want` is the
  // reference's low-32 value for that register.
  const RegOperand exp = ClassifyReg(expected);
  if (exp.is_reg) {
    PinReg32(rig, exp, want, name);
  }
}

inline void AssertEqualNzcv(LiveRig& rig, std::uint32_t expected) {
  if (Current().skipped || !Current().ran) {
    return;
  }
  std::uint32_t ref_val = rig.core.flags_nzcv();
  if (ref_val != expected) {
    MarkSkip(
        "reference sim disagrees with the upstream ASSERT_EQUAL_NZCV value");
    return;
  }
  const char* name = Current().name.c_str();
  CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
    return CheckNzcv(s, ref_val, trk, name);
  });
}

// ASSERT_EQUAL_FP16(expected, hN): expected is a vixl::Float16; compare the H
// register's raw 16 bits. gaby's imported simulator models FP16 leaves fully,
// so these run on all three engines — only the absolute anchoring was
// previously dropped (a forced skip), which is the coverage this restores.
template <class Res>
void AssertEqualFP16(LiveRig& rig, vixl::Float16 expected, const Res& result) {
  if (Current().skipped || !Current().ran) {
    return;
  }
  unsigned code = 0;
  if (!ResultRegCode(result, code)) {
    return;
  }
  std::uint16_t want = vixl::Float16ToRawbits(expected);
  std::uint16_t ref_val = rig.core.hreg_bits(code);
  if (ref_val != want) {
    MarkSkip(
        "reference sim disagrees with the upstream ASSERT_EQUAL_FP16 value");
    return;
  }
  const char* name = Current().name.c_str();
  CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
    return CheckFP16(s, code, ref_val, trk, name);
  });
}

template <class Res>
void AssertEqualFP32(LiveRig& rig, float expected, const Res& result) {
  if (Current().skipped || !Current().ran) {
    return;
  }
  unsigned code = 0;
  if (!ResultRegCode(result, code)) {
    return;
  }
  std::uint32_t want = vixl::FloatToRawbits(expected);
  std::uint32_t ref_val = rig.core.sreg_bits(code);
  if (ref_val != want) {
    MarkSkip(
        "reference sim disagrees with the upstream ASSERT_EQUAL_FP32 value");
    return;
  }
  const char* name = Current().name.c_str();
  CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
    return CheckFP32(s, code, ref_val, trk, name);
  });
}

template <class Res>
void AssertEqualFP64(LiveRig& rig, double expected, const Res& result) {
  if (Current().skipped || !Current().ran) {
    return;
  }
  unsigned code = 0;
  if (!ResultRegCode(result, code)) {
    return;
  }
  std::uint64_t want = vixl::DoubleToRawbits(expected);
  std::uint64_t ref_val = rig.core.dreg_bits(code);
  if (ref_val != want) {
    MarkSkip(
        "reference sim disagrees with the upstream ASSERT_EQUAL_FP64 value");
    return;
  }
  const char* name = Current().name.c_str();
  CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
    return CheckFP64(s, code, ref_val, trk, name);
  });
}

template <class Res>
void AssertEqual128(LiveRig& rig,
                    std::uint64_t expected_hi,
                    std::uint64_t expected_lo,
                    const Res& result) {
  if (Current().skipped || !Current().ran) {
    return;
  }
  unsigned code = 0;
  if (!ResultRegCode(result, code)) {
    return;
  }
  std::uint64_t ref_lo = rig.ref_state.v[code].lo;
  std::uint64_t ref_hi = rig.ref_state.v[code].hi;
  if (ref_lo != expected_lo || ref_hi != expected_hi) {
    MarkSkip(
        "reference sim disagrees with the upstream ASSERT_EQUAL_128 value");
    return;
  }
  const char* name = Current().name.c_str();
  CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
    return CheckV128(s, code, ref_lo, ref_hi, trk, name);
  });
}

// ASSERT_NOT_EQUAL_64(forbidden, result): upstream asserts that `result`
// (always a register in the ported bodies) differs from `forbidden`, which
// itself is EITHER an immediate OR another register. The gaby property is "both
// registers hold the reference sim's values"; since the reference satisfies
// result != forbidden, pinning gaby to those values reproduces the inequality
// and is strictly stronger than a bare "!=".
//
// Both register operands must be pinned. Pinning only `result` would miss the
// case the upstream invariant is meant to catch: if both gaby tracks wrongly
// compute `forbidden` (a register) equal to a correct `result`, "result !=
// forbidden" is violated in gaby yet `result` alone still matches the
// reference. So when `forbidden` is also a register, its gaby value is pinned
// to the reference too. (An immediate `forbidden` has no register to pin; the
// consistency check below then guarantees ref.result != forbidden, and
// gaby.result == ref.result carries the inequality.)
//
// If the reference's `result` equals `forbidden`, the upstream invariant does
// not hold under the harness's addresses (not a gaby divergence) — skip.
template <class Exp, class Res>
void AssertNotEqual64(LiveRig& rig, const Exp& forbidden, const Res& result) {
  if (Current().skipped || !Current().ran) {
    return;
  }
  unsigned res_code = 0;
  if (!ResultRegCode(result, res_code)) {
    return;  // result not a register — nothing to pin (no such body today)
  }
  std::uint64_t ref_result =
      static_cast<std::uint64_t>(rig.core.xreg(res_code));

  unsigned forb_code = 0;
  const bool forbidden_is_reg = ResultRegCode(forbidden, forb_code);
  bool forbidden_ok = false;
  std::uint64_t bad = ResolveExpected(forbidden, rig.core, forbidden_ok);
  if (forbidden_ok && ref_result == bad) {
    MarkSkip(
        "reference sim's value equals the ASSERT_NOT_EQUAL_64 'forbidden' "
        "value");
    return;
  }

  const char* name = Current().name.c_str();
  CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
    return CheckX(s, res_code, ref_result, trk, name);
  });
  if (forbidden_is_reg) {
    // `bad` == the reference's value for the forbidden register
    // (ResolveExpected reads core.xreg(forb_code) for a register operand). Pin
    // gaby to it too.
    CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
      return CheckX(s, forb_code, bad, trk, name);
    });
  }
}

// ASSERT_EQUAL_REGISTERS(before): the body asserts no register changed since a
// mid-body RegisterDump. The `before` snapshot is intentionally ignored — the
// reference sim ran the identical body bytes from the identical seeded entry,
// so its body-exit register file IS the ground truth for what every register
// should be (equivalent to `before`, since both engines are entry-unified and
// run the same instructions). Pin both gaby tracks' full register file
// (x0..x30, sp, all V; NZCV is left to the body's separate ASSERT_EQUAL_NZCV,
// matching upstream EqualRegisters semantics) to the reference. Only zero_dest
// / zero_dest_setflags reach here — the other two upstream uses (printf /
// system) are quarantined.
template <class Dump>
void AssertEqualRegisters(LiveRig& rig, const Dump&) {
  if (Current().skipped || !Current().ran) {
    return;
  }
  const char* name = Current().name.c_str();
  for (unsigned i = 0; i < 31; ++i) {
    std::uint64_t want = rig.ref_state.x[i];
    CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
      return CheckX(s, i, want, trk, name);
    });
  }
  std::uint64_t want_sp = rig.ref_state.sp;
  CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
    return CheckX(s, 31, want_sp, trk, name);  // code 31 -> sp in the snapshot
  });
  for (unsigned i = 0; i < 32; ++i) {
    std::uint64_t lo = rig.ref_state.v[i].lo;
    std::uint64_t hi = rig.ref_state.v[i].hi;
    CheckBothTracks(rig, [&](const gaby_vm::RegisterFile& s, const char* trk) {
      return CheckV128(s, i, lo, hi, trk, name);
    });
  }
}

}  // namespace vixl_port_live
}  // namespace gaby_vm

// --- Macro surface ---------------------------------------------------------
// The names/bodies upstream test .cc expect. `rig_` is the per-test LiveRig;
// `masm` / `core` / `simulator` alias into it so body expressions touching them
// compile unchanged. `simulator` aliases the REFERENCE sim (some bodies query
// it for feature checks); gaby execution is driven by RUN(), not the body.

#define __ masm.

#define TEST(name) TEST_(AARCH64_ASM_##name)

#define SETUP()                                              \
  ::gaby_vm::vixl_port_live::LiveRig rig_;                   \
  vixl::aarch64::MacroAssembler& masm = rig_.masm;           \
  vixl::aarch64::Simulator& simulator = rig_.eng.ref_sim;    \
  ::gaby_vm::vixl_port_live::RefStateView& core = rig_.core; \
  (void)simulator;                                           \
  (void)core

// gaby registers under All(); feature restrictions are filtered by *seen*
// features after the reference run, so the feature arguments are ignored here.
#define SETUP_WITH_FEATURES(...) SETUP()
#define SETUP_CUSTOM(size, pic) SETUP()
#define SETUP_CUSTOM_SIM(...) SETUP()

// The reference sim is REUSED across cases (constructing one is ~100ms in
// debug). Several pieces of its state therefore MUST be reset per case or they
// leak (Simulator::ResetState() does NOT cover them — it resets architectural
// registers, not these sticky knobs):
//   * CPUFeatures — a configure_cpu_features test restricts them mid-body;
//   * the auditor's seen-feature set — it accumulates, so without a reset every
//     case after the first MTE/PAuth/... body would be wrongly feature-denied.
//   * guarded-page mode — a BTI body calls SetGuardedPages(true) and never
//     clears it; ResetState() leaves guard_pages_ untouched, so without this
//     reset every later body with a plain BR/BLR (e.g. blr_lr, branch_tagged,
//     branch_and_link_tagged) would abort in the reference sim with a wrong-
//     BType check and be wrongly skipped. (gaby's tracks would then never run.)
// (The extraction tool used a fresh sim per case and so never hit this.)
#define START()                                                         \
  masm.Reset();                                                         \
  rig_.eng.ref_sim.ResetState();                                        \
  rig_.eng.ref_sim.SetCPUFeatures(vixl::CPUFeatures::All());            \
  rig_.eng.ref_sim.ResetSeenFeatures();                                 \
  rig_.eng.ref_sim.SetGuardedPages(false);                              \
  rig_.eng.ref_sim.DisableGCSCheck();                                   \
  rig_.eng.ref_sim.WriteSp(                                             \
      ::gaby_vm::vixl_port_live::StackTop(rig_.eng.gaby_stack.data(),   \
                                          rig_.eng.gaby_stack.size())); \
  rig_.eng.ref_sim.WriteXRegister(30, 0);                               \
  masm.SetCPUFeatures(vixl::CPUFeatures::All());                        \
  masm.SetGenerateSimulatorCode(true);                                  \
  ::gaby_vm::vixl_port_live::SnapshotEntry(rig_);                       \
  rig_.body_start = masm.GetCursorOffset()

// Force any pending literal pool inline (with a branch over it) so LDR-literal
// loads travel with the body, then record body_end and finalize. No absolute-
// oracle dump is emitted (the reference's body-exit registers are read directly
// in SnapshotRef) and no terminator is emitted here — the gaby slice is
// [body_start, body_end) and RUN() appends a raw br xzr to it, which all three
// engines run from the same address.
#define END()                                                        \
  masm.EmitLiteralPool(vixl::aarch64::LiteralPool::kBranchRequired); \
  rig_.body_end = masm.GetCursorOffset();                            \
  masm.FinalizeCode()

// __builtin_frame_address(0) here is the BODY function's frame pointer (this
// macro inlines into the body); TwoTrackRun uses it as the upper bound of the
// frame window it snapshots/restores between engine runs (see TwoTrackRun).
#define RUN() \
  ::gaby_vm::vixl_port_live::TwoTrackRun(rig_, __builtin_frame_address(0))
#define RUN_WITHOUT_SEEN_FEATURE_CHECK() RUN()

#define CAN_RUN() true
#define QUERIED_CAN_RUN() true
#define DISASSEMBLE() ((void)0)

#define ASSERT_EQUAL_64(expected, result) \
  ::gaby_vm::vixl_port_live::AssertEqual64(rig_, (expected), (result))
#define ASSERT_EQUAL_32(expected, result) \
  ::gaby_vm::vixl_port_live::AssertEqual32(rig_, (expected), (result))
#define ASSERT_EQUAL_NZCV(expected) \
  ::gaby_vm::vixl_port_live::AssertEqualNzcv(rig_, (expected))
#define ASSERT_EQUAL_FP16(expected, result) \
  ::gaby_vm::vixl_port_live::AssertEqualFP16(rig_, (expected), (result))
#define ASSERT_EQUAL_FP32(expected, result) \
  ::gaby_vm::vixl_port_live::AssertEqualFP32(rig_, (expected), (result))
#define ASSERT_EQUAL_FP64(expected, result) \
  ::gaby_vm::vixl_port_live::AssertEqualFP64(rig_, (expected), (result))
#define ASSERT_EQUAL_128(expected_h, expected_l, result)  \
  ::gaby_vm::vixl_port_live::AssertEqual128(rig_,         \
                                            (expected_h), \
                                            (expected_l), \
                                            (result))

#define ASSERT_NOT_EQUAL_64(expected, result) \
  ::gaby_vm::vixl_port_live::AssertNotEqual64(rig_, (expected), (result))
#define ASSERT_EQUAL_REGISTERS(expected) \
  ::gaby_vm::vixl_port_live::AssertEqualRegisters(rig_, (expected))

// ASSERT_LITERAL_POOL_SIZE is the one assert deliberately left a no-op: it
// inspects the test-only island MacroAssembler's pending literal-pool byte
// count — an assembler property, NOT a gaby leaf-execution property — so it is
// out of scope for the two-track simulator oracle. The pure
// pool/code-generation bodies that hinge on it never call RUN() (counted as
// skipped, "body never invoked RUN()"); the mixed ones (ldr_literal_*) also
// carry real register asserts the absolute oracle checks. So a no-op here never
// lets an unverified case count as passed. (The shared leaf is exercised by the
// register + memory oracles, not by re-verifying upstream's unmodified
// assembler against itself.)
#define ASSERT_LITERAL_POOL_SIZE(expected) ((void)0)

// Forms not turned into oracle checks. They must still COMPILE (the whole
// upstream .cc is one TU), so they expand to a skip mark.
//
// ASSERT_EQUAL_MEMORY is unused in the ported (non-SVE) bodies — upstream only
// uses it inside SVE tests, which we do not run. If a future upstream body did
// reach it, store side-effects are already covered more strongly by the
// frame-window memory oracle in TwoTrackRun (all three engines' exit images
// compared), so a no-op skip here loses no coverage. Kept as a skip mark only
// so an unexpected use is visible rather than silently passing.
#define ASSERT_EQUAL_MEMORY(expected, result, ...)         \
  ::gaby_vm::vixl_port_live::MarkSkip(                     \
      "ASSERT_EQUAL_MEMORY (store results covered by the " \
      "frame-window memory oracle)")
#define ASSERT_EQUAL_SVE_LANE(expected, result, lane) \
  ::gaby_vm::vixl_port_live::MarkSkip("SVE assert unsupported")
#define ASSERT_EQUAL_SVE(expected, result) \
  ::gaby_vm::vixl_port_live::MarkSkip("SVE assert unsupported")

#define MUST_FAIL_WITH_MESSAGE(code, message) \
  ::gaby_vm::vixl_port_live::MarkSkip("MUST_FAIL negative test")

#endif  // GABY_VM_TEST_VIXL_ASM_GABY_TWO_TRACK_MACROS_H_
