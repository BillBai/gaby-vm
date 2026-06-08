// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Redefines VIXL's test macros so a VIXL TEST() body, compiled verbatim,
// CAPTURES (body bytes + entry state + ASSERT targets) instead of asserting.
//
// Mechanism (see docs/refs/gaby-vm-vixl-sim-test-port-design-2026-06-08.md).
// VIXL's test-assembler-aarch64.h has NO include guard, so the "occupy the
// guard" trick is impossible. Instead the build feeds us a copy of the upstream
// test .cc with its `#include "test-assembler-aarch64.h"` line stripped
// (see CMakeLists.txt), and this header — included first — supplies the macros.
//
// Capture runs in *simulator mode*. The simulated CPU has its own register
// file, so the VIXL prologue (PushCalleeSavedRegisters) and the infrastructure
// HLT pseudo-ops (SimulationCPUFeaturesScope / MTE markers / Trace) — which
// exist only for native execution and feature scoping — are unnecessary. We
// therefore emit NO prologue: the body starts at buffer offset 0, entry state
// is the simulator's post-ResetState snapshot (read directly, never via emitted
// Dump code), and the absolute-oracle anchor is VIXL's own `core.Dump()`.
#ifndef GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_MACROS_H_
#define GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_MACROS_H_

#include <cstdio>
#include <sstream>
#include <type_traits>
#include <utility>

#include "capture_state.h"
#include "test-runner.h"  // vixl::Test + TEST_()

#include "aarch64/cpu-features-auditor-aarch64.h"
#include "aarch64/macro-assembler-aarch64.h"
#include "aarch64/simulator-aarch64.h"
#include "aarch64/test-utils-aarch64.h"  // RegisterDump + Equal* helpers

namespace vixl {
namespace aarch64 {

// Guest-visible output (the simulator's `Printf` runtime call) is routed here
// so printf-family tests do not spam stdout during extraction. Opened once.
inline std::FILE* CaptureNullStream() {
  static std::FILE* f = std::fopen("/dev/null", "w");
  return f ? f : stderr;
}

// Per-case captured machinery, replacing SETUP()'s bare locals.
struct CaptureRig {
  MacroAssembler masm;
  Decoder decoder;
  Simulator simulator;
  RegisterDump core;  // body-exit state (VIXL's `core`), the absolute anchor
  ptrdiff_t body_start = 0;
  ptrdiff_t body_end = 0;

  CaptureRig() : simulator(&decoder, CaptureNullStream()) {
    // gaby-vm registers code under CPUFeatures::All(); mirror that here so no
    // body is gated out at assemble/run time. Filtering is by *seen* features.
    simulator.SetCPUFeatures(CPUFeatures::All());
  }
};

}  // namespace aarch64
}  // namespace vixl

namespace gaby_vm {
namespace extract {

// --- Entry / run / harvest -------------------------------------------------

// Snapshot the simulator's post-ResetState architectural state. Called from
// START(), before any execution, so it is exactly what the body sees at its
// first instruction (there is no prologue between ResetState and the body).
inline void CaptureEntry(vixl::aarch64::CaptureRig& rig) {
  CapturedCase& c = Current();
  for (unsigned i = 0; i < 31; ++i) {
    c.entry.x[i] = static_cast<uint64_t>(rig.simulator.ReadXRegister(i));
  }
  // sp and the LR slot are overridden by the replay runner; do not bake the
  // simulator's host stack base / end-of-sim sentinel into the fixture.
  c.entry.x[30] = 0;
  c.entry.sp = 0;
  for (unsigned i = 0; i < 32; ++i) {
    vixl::aarch64::SimVRegister& v = rig.simulator.ReadVRegister(i);
    c.entry.v_lo[i] = v.GetLane<uint64_t>(0);
    c.entry.v_hi[i] = v.GetLane<uint64_t>(1);
  }
  c.entry.nzcv = rig.simulator.ReadNzcv().GetRawValue();
  c.entry.fpcr = rig.simulator.ReadFpcr().GetRawValue();
  c.entry.fpsr = 0;  // the imported simulator does not model FPSR.
}

// True if `word` is an A64 instruction whose execution is not portable to a
// relocated, different-process replay, classified purely by top-level encoding
// bits (robust to the exact opcode; survives VIXL upgrades):
//   * load/store group — tests bake host buffer addresses; gaby would fault;
//   * ADR/ADRP         — produce a host-PC-relative value;
//   * register-indirect branch (BR/BLR/RET and PAC variants) — branch to a
//     register that may hold a non-relocatable / poison address, after which
//     the decoder track fetches at a wild PC and (ASLR-dependently) crashes.
// PC-relative branches (B/BL/B.cond/CBZ/TBZ) are NOT flagged: their targets are
// fixed relative offsets the assembler kept inside the body.
inline bool IsNonPortableInstr(uint32_t word) {
  // Loads and stores: op1 (bits[28:25]) == x1x0, i.e. bit27 set and bit25
  // clear. Covers LDR/STR/LDP/STP/LDUR/STUR, exclusives, atomics, CAS and
  // load-literal.
  const bool load_store = ((word >> 27) & 1u) && !((word >> 25) & 1u);
  // PC-relative addressing (ADR/ADRP): bits[28:24] == 0b10000.
  const bool pc_rel = ((word >> 24) & 0x1fu) == 0x10u;
  // Unconditional branch (register) group: bits[31:25] == 0b1101011.
  const bool br_reg = (word >> 25) == 0x6bu;
  // SYS / SYSL (the DC/IC/AT/TLBI cache/TLB-maintenance aliases): bits[31:19]
  // == 0x1AA1 (SYS) or 0x1AA9 (SYSL). These operate on a virtual address, so
  // tests pass a host pointer and gaby would fault. MSR/MRS, barriers and HINTs
  // use distinct top-13-bit values and are NOT matched.
  const bool sys = (word >> 19) == 0x1aa1u || (word >> 19) == 0x1aa9u;
  return load_store || pc_rel || br_reg || sys;
}

// Run the fully emitted program (body + core.Dump + Br(xzr)) on the real VIXL
// simulator, then harvest the body bytes and the seen-feature set. The Br(xzr)
// branches to address 0, the kEndOfSimAddress sentinel, so the run terminates.
inline void HarvestRun(vixl::aarch64::CaptureRig& rig) {
  using vixl::aarch64::Instruction;
  CapturedCase& c = Current();
  c.run_count += 1;
  if (c.run_count > 1) {
    // Some TESTs drive several SETUP/RUN cycles (e.g. via a helper called in a
    // loop). Our single-case model cannot represent those; skip without running
    // again (also avoids a second hang).
    c.skipped = true;
    c.skip_reason = "multiple RUN() in one TEST";
    return;
  }

  // Open-coded RunFrom with a deterministic instruction cap. Two reasons:
  //  1. Some bodies loop on a count register that, without VIXL's prologue,
  //     holds the ResetState poison (0xbadbeef ~= 195M) and would spin forever.
  //  2. A body that executes a large loop is a poor guard-rail fixture — it
  //     exercises one leaf 10^5 times and replays slowly on the debug decoder
  //     track. Real register/ALU tests execute well under 1000 instructions.
  // The cap bounds the run and marks such cases unportable for the manifest.
  constexpr uint64_t kMaxInstructions = 8'192;
  rig.simulator.WritePc(
      rig.masm.GetBuffer()->GetStartAddress<const Instruction*>());
  uint64_t executed = 0;
  while (!rig.simulator.IsSimulationFinished()) {
    if (executed++ >= kMaxInstructions) {
      c.skipped = true;
      c.skip_reason = "instruction cap exceeded (looping or multi-RUN body)";
      return;
    }
    rig.simulator.ExecuteInstruction();
  }

  const auto* base = rig.masm.GetBuffer()->GetStartAddress<const uint32_t*>();
  size_t first = static_cast<size_t>(rig.body_start) / sizeof(uint32_t);
  size_t last = static_cast<size_t>(rig.body_end) / sizeof(uint32_t);
  c.body_words.clear();
  bool non_portable = false;
  for (size_t i = first; i < last; ++i) {
    c.body_words.push_back(base[i]);
    if (IsNonPortableInstr(base[i])) {
      non_portable = true;
    }
  }

  // Record this case's code-buffer host range so the writer can drop ASSERT
  // targets whose expected value is a host address.
  c.code_lo = rig.masm.GetBuffer()->GetStartAddress<uintptr_t>();
  c.code_hi = rig.masm.GetBuffer()->GetEndAddress<uintptr_t>();

  std::ostringstream os;
  os << rig.simulator.GetSeenFeatures();
  c.required_features = os.str();

  if (non_portable) {
    c.skipped = true;
    c.skip_reason =
        "body uses load/store, PC-relative addressing, or a register-indirect "
        "branch (host-memory/host-address-dependent; not portable to replay)";
    return;
  }

  // Far-branch-range tests emit hundreds of thousands of padding instructions
  // to overflow a branch's reach. Such a body is absurd as a guard-rail fixture
  // (it bloats the committed .inc and is slow to predecode), and it exercises
  // assembler range handling rather than a leaf's semantics. Cap the size: real
  // ported bodies are well under 100 words.
  constexpr size_t kMaxBodyWords = 4096;
  if (c.body_words.size() > kMaxBodyWords) {
    c.skipped = true;
    c.skip_reason = "body too large (far-branch range padding)";
  }
}

// --- ASSERT_EQUAL_* recorders ----------------------------------------------
//
// The upstream ASSERT_EQUAL_64 / _32 admit many argument shapes across 280+
// tests:
//   ASSERT_EQUAL_64(literal, xreg)            — the common, replayable case
//   ASSERT_EQUAL_64(xreg, xreg)               — expected is itself a register
//   ASSERT_EQUAL_64(literal, computed_expr)   — result is not a register
//   ASSERT_EQUAL_64(std::vector<...>, xreg)   — exotic "set of allowed" forms
// To keep the whole upstream .cc compiling while only turning the replayable
// shapes into AssertTargets, the operands are classified by DUCK TYPING:
// anything with .GetCode() is a register, anything convertible to uint64_t is a
// scalar, anything else makes the assert non-replayable (dropped, not a
// target).

template <class T, class = void>
struct HasGetCode : std::false_type {};
template <class T>
struct HasGetCode<T, std::void_t<decltype(std::declval<const T&>().GetCode())>>
    : std::true_type {};

// Resolve an "expected" operand to bits. ok=false => not a value we can use.
template <class T>
uint64_t ResolveExpected(const T& v,
                         vixl::aarch64::RegisterDump& core,
                         bool& ok) {
  if constexpr (HasGetCode<T>::value) {
    ok = true;
    return static_cast<uint64_t>(core.xreg(v.GetCode()));
  } else if constexpr (std::is_convertible_v<T, uint64_t>) {
    ok = true;
    return static_cast<uint64_t>(v);
  } else {
    ok = false;
    return 0;
  }
}

// Extract a register code from a "result" operand. false => not a register.
template <class T>
bool ResultRegCode(const T& r, unsigned& code) {
  if constexpr (HasGetCode<T>::value) {
    code = r.GetCode();
    return true;
  } else {
    return false;
  }
}

template <class Exp, class Res>
void RecordEqual64(vixl::aarch64::RegisterDump& core,
                   const Exp& expected,
                   const Res& result) {
  bool exp_ok = false;
  uint64_t want = ResolveExpected(expected, core, exp_ok);
  unsigned code = 0;
  if (!exp_ok || !ResultRegCode(result, code)) {
    Current().dropped_asserts += 1;
    return;
  }
  if (static_cast<uint64_t>(core.xreg(code)) != want) {
    Current().skipped = true;
    Current().skip_reason = "self-check: ASSERT_EQUAL_64 mismatch under VIXL";
  }
  Current().asserts.push_back(
      {AssertKind::kX, static_cast<uint8_t>(code), want, 0});
}

template <class Exp, class Res>
void RecordEqual32(vixl::aarch64::RegisterDump& core,
                   const Exp& expected,
                   const Res& result) {
  bool exp_ok = false;
  uint64_t want = ResolveExpected(expected, core, exp_ok) & 0xffffffffu;
  unsigned code = 0;
  if (!exp_ok || !ResultRegCode(result, code)) {
    Current().dropped_asserts += 1;
    return;
  }
  if ((static_cast<uint64_t>(core.wreg(code)) & 0xffffffffu) != want) {
    Current().skipped = true;
    Current().skip_reason = "self-check: ASSERT_EQUAL_32 mismatch under VIXL";
  }
  Current().asserts.push_back(
      {AssertKind::kW, static_cast<uint8_t>(code), want, 0});
}

inline void RecordNzcv(vixl::aarch64::RegisterDump& core, uint32_t expected) {
  if (core.flags_nzcv() != expected) {
    Current().skipped = true;
    Current().skip_reason = "self-check: ASSERT_EQUAL_NZCV mismatch under VIXL";
  }
  Current().asserts.push_back({AssertKind::kNZCV, 0, expected, 0});
}

inline void RecordFP32(vixl::aarch64::RegisterDump& core,
                       float expected,
                       unsigned code) {
  uint32_t bits = vixl::FloatToRawbits(expected);
  if (core.sreg_bits(code) != bits) {
    Current().skipped = true;
    Current().skip_reason = "self-check: ASSERT_EQUAL_FP32 mismatch under VIXL";
  }
  Current().asserts.push_back(
      {AssertKind::kFP32, static_cast<uint8_t>(code), bits, 0});
}

inline void RecordFP64(vixl::aarch64::RegisterDump& core,
                       double expected,
                       unsigned code) {
  uint64_t bits = vixl::DoubleToRawbits(expected);
  if (core.dreg_bits(code) != bits) {
    Current().skipped = true;
    Current().skip_reason = "self-check: ASSERT_EQUAL_FP64 mismatch under VIXL";
  }
  Current().asserts.push_back(
      {AssertKind::kFP64, static_cast<uint8_t>(code), bits, 0});
}

inline void RecordV128(vixl::aarch64::RegisterDump& core,
                       uint64_t expected_hi,
                       uint64_t expected_lo,
                       unsigned code) {
  vixl::aarch64::QRegisterValue q = core.qreg(code);
  if (q.GetLane<uint64_t>(0) != expected_lo ||
      q.GetLane<uint64_t>(1) != expected_hi) {
    Current().skipped = true;
    Current().skip_reason = "self-check: ASSERT_EQUAL_128 mismatch under VIXL";
  }
  Current().asserts.push_back({AssertKind::kV128,
                               static_cast<uint8_t>(code),
                               expected_lo,
                               expected_hi});
}

// Mark the in-flight case unportable (an assert form / setup we cannot replay).
inline void MarkUnsupported(const char* reason) {
  Current().skipped = true;
  Current().skip_reason = reason;
}

}  // namespace extract
}  // namespace gaby_vm

// --- Macro surface ---------------------------------------------------------
// The names and bodies the upstream test .cc expects. `rig_` is the per-test
// CaptureRig; `masm` / `core` / `simulator` are references into it so body
// expressions that touch them compile unchanged.

#define __ masm.

#define TEST(name) TEST_(AARCH64_ASM_##name)

#define SETUP()                                         \
  vixl::aarch64::CaptureRig rig_;                       \
  vixl::aarch64::MacroAssembler& masm = rig_.masm;      \
  vixl::aarch64::Simulator& simulator = rig_.simulator; \
  vixl::aarch64::RegisterDump& core = rig_.core;        \
  (void)simulator;                                      \
  (void)core

// gaby-vm registers under All(); feature restrictions are not modelled at
// capture time (we filter by *seen* features afterwards), so the feature
// arguments are intentionally ignored.
#define SETUP_WITH_FEATURES(...) SETUP()
#define SETUP_CUSTOM(size, pic) SETUP()
#define SETUP_CUSTOM_SIM(...) SETUP()

#define START()                                  \
  masm.Reset();                                  \
  simulator.ResetState();                        \
  masm.SetCPUFeatures(vixl::CPUFeatures::All()); \
  masm.SetGenerateSimulatorCode(true);           \
  ::gaby_vm::extract::CaptureEntry(rig_);        \
  rig_.body_start = masm.GetCursorOffset()

// Dump body-exit state, then terminate via `Br(xzr)` (branch to register 31 =
// the zero register = address 0, VIXL's end-of-sim sentinel). This is immune to
// bodies that clobber LR (e.g. `Mov(x30, ...)`): unlike Ret, it does not read
// x30. No prologue/epilogue is emitted, so the body slice stays pure.
#define END()                             \
  rig_.body_end = masm.GetCursorOffset(); \
  core.Dump(&masm);                       \
  masm.Br(xzr);                           \
  masm.FinalizeCode()

#define RUN() ::gaby_vm::extract::HarvestRun(rig_)
#define RUN_WITHOUT_SEEN_FEATURE_CHECK() RUN()

#define CAN_RUN() true
#define QUERIED_CAN_RUN() true
#define DISASSEMBLE() ((void)0)

#define ASSERT_EQUAL_64(expected, result) \
  ::gaby_vm::extract::RecordEqual64(core, (expected), (result))
#define ASSERT_EQUAL_32(expected, result) \
  ::gaby_vm::extract::RecordEqual32(core, (expected), (result))
#define ASSERT_EQUAL_NZCV(expected) \
  ::gaby_vm::extract::RecordNzcv(core, (expected))
#define ASSERT_EQUAL_FP32(expected, result) \
  ::gaby_vm::extract::RecordFP32(core, (expected), (result).GetCode())
#define ASSERT_EQUAL_FP64(expected, result) \
  ::gaby_vm::extract::RecordFP64(core, (expected), (result).GetCode())
#define ASSERT_EQUAL_128(expected_h, expected_l, result) \
  ::gaby_vm::extract::RecordV128(core,                   \
                                 (expected_h),           \
                                 (expected_l),           \
                                 (result).GetCode())

// Forms we do not turn into replayable targets. They must still COMPILE (the
// whole upstream .cc is one TU), so they expand to a no-op or a skip mark.
#define ASSERT_NOT_EQUAL_64(expected, result) ((void)0)
#define ASSERT_EQUAL_REGISTERS(expected) ((void)0)
#define ASSERT_LITERAL_POOL_SIZE(expected) ((void)0)
#define ASSERT_EQUAL_FP16(expected, result) \
  ::gaby_vm::extract::MarkUnsupported("ASSERT_EQUAL_FP16 unsupported")
#define ASSERT_EQUAL_MEMORY(expected, result, ...) \
  ::gaby_vm::extract::MarkUnsupported("ASSERT_EQUAL_MEMORY unsupported")
#define ASSERT_EQUAL_SVE_LANE(expected, result, lane) \
  ::gaby_vm::extract::MarkUnsupported("SVE assert unsupported")
#define ASSERT_EQUAL_SVE(expected, result) \
  ::gaby_vm::extract::MarkUnsupported("SVE assert unsupported")

// Negative tests: do NOT execute `code` (it is meant to abort); mark skip.
#define MUST_FAIL_WITH_MESSAGE(code, message) \
  ::gaby_vm::extract::MarkUnsupported("MUST_FAIL negative test")

#endif  // GABY_VM_TOOLS_VIXL_TEST_EXTRACT_CAPTURE_MACROS_H_
