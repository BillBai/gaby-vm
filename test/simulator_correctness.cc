// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// simulator_correctness: AArch64 leaf-semantics regression test for the
// imported VIXL simulator.
//
// Why hand-encoded uint32_t literals (no assembler).
// --------------------------------------------------
// The imported VIXL tree is bounded to Tiers 1+2+3 of the extraction map; the
// assembler / macro-assembler (Tier 0) is deliberately not imported, and the
// project does not link any other in-tree assembler. Each instruction word
// below is therefore a literal hex constant, with the AArch64 mnemonic in an
// inline comment. Hand-typed encodings are easy to get wrong (a one-bit field
// slip is invisible to the eye), so every encoding is verified at authorship
// time via an external assembler before commit:
//
//     printf 'add x0, x1, x2\nret\n' \
//         | clang -target arm64-apple-darwin -c -x assembler -o /tmp/enc.o -
//     otool -t /tmp/enc.o
//     # The hex columns after the address are the 32-bit instruction words.
//     # Copy each into this file as a uint32_t literal and update the
//     # mnemonic comment beside it.
//
// The clang+otool pair is the verification flow; both ship with Xcode
// CommandLineTools, so verification has zero install cost on macOS dev hosts.
// (`llvm-mc -triple=aarch64 -show-encoding` produces equivalent information
// when available.) These tools are never invoked by the build or test — they
// exist only at authorship time. The runtime test binary contains only
// uint32_t literals.
//
// Termination protocol.
// ---------------------
// VIXL's Simulator::ResetRegisters() initializes the link register to
// kEndOfSimAddress (== nullptr). Simulator::Run() exits when
// pc_ == kEndOfSimAddress. Every encoded sequence in this file therefore ends
// with RET (0xd65f03c0): the implicit LR == NULL set up at Simulator
// construction is the termination contract we rely on. BL+RET sub-tests
// preserve LR across the inner call so the outer RET still terminates the
// simulation (see run_call_return_tests).
// =============================================================================

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cpu-features.h"

#include "aarch64/decoder-aarch64.h"
#include "aarch64/simulator-aarch64.h"

namespace {

using vixl::CPUFeatures;
using vixl::aarch64::Decoder;
using vixl::aarch64::Instruction;
using vixl::aarch64::Register;
using vixl::aarch64::Simulator;

// Named GPR objects from VIXL (registers-aarch64.h:813, defined by macro
// expansion of `VIXL_DEFINE_REGISTERS(N)`). Imported here so test code says
// `x0`, `x1`, `x2` instead of magic integer codes 0, 1, 2.
using vixl::aarch64::x0;
using vixl::aarch64::x1;
using vixl::aarch64::x2;

// -------- Register access (thin wrappers over the unsigned-code API) --------
//
// Simulator's Read/WriteXRegister take `unsigned code`, not a Register object,
// so the .GetCode() call must live somewhere. Concentrating it in these
// helpers lets every call site use VIXL's named Register objects directly,
// keeping "which register" visible at the type-system level.
void write_x(Simulator& sim, const Register& r, uint64_t v) {
  sim.WriteXRegister(r.GetCode(), static_cast<int64_t>(v));
}
uint64_t read_x(const Simulator& sim, const Register& r) {
  return static_cast<uint64_t>(sim.ReadXRegister(r.GetCode()));
}

// -------- Test state --------

struct TestState {
  int passed = 0;
  int total = 0;
  const char* current_family = "";
  const char* current_subtest = "";
};

// -------- Assertion helpers --------

bool expect_eq_u64(TestState& s,
                   uint64_t actual,
                   uint64_t expected,
                   const char* label) {
  s.total += 1;
  if (actual == expected) {
    s.passed += 1;
    return true;
  }
  std::fprintf(stderr,
               "[FAIL] %s / %s: %s\n"
               "  actual   = 0x%016llx (%llu)\n"
               "  expected = 0x%016llx (%llu)\n",
               s.current_family,
               s.current_subtest,
               label,
               static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(expected),
               static_cast<unsigned long long>(expected));
  return false;
}

bool expect_eq_u32(TestState& s,
                   uint32_t actual,
                   uint32_t expected,
                   const char* label) {
  s.total += 1;
  if (actual == expected) {
    s.passed += 1;
    return true;
  }
  std::fprintf(stderr,
               "[FAIL] %s / %s: %s\n"
               "  actual   = 0x%08x (%u)\n"
               "  expected = 0x%08x (%u)\n",
               s.current_family,
               s.current_subtest,
               label,
               actual,
               actual,
               expected,
               expected);
  return false;
}

bool expect_eq_u8(TestState& s,
                  uint8_t actual,
                  uint8_t expected,
                  const char* label) {
  s.total += 1;
  if (actual == expected) {
    s.passed += 1;
    return true;
  }
  std::fprintf(stderr,
               "[FAIL] %s / %s: %s\n"
               "  actual   = 0x%02x (%u)\n"
               "  expected = 0x%02x (%u)\n",
               s.current_family,
               s.current_subtest,
               label,
               actual,
               actual,
               expected,
               expected);
  return false;
}

// -------- Simulator configuration helper --------
//
// The CPU-features auditor inside ExecuteInstruction() rejects any instruction
// whose feature isn't enabled. SetCPUFeatures(All()) right after construction
// avoids that. This helper is the single point that decides which feature set
// to enable, so no sub-test can forget the call.
void configure_simulator(Simulator& sim) {
  sim.SetCPUFeatures(CPUFeatures::All());
}

// -------- Family stubs (filled in by groups 4-8 of the change) --------

void run_arithmetic_tests(TestState& s) {
  s.current_family = "arithmetic";

  // ADD x0, x1, x2 — register-register form (no shift).
  // Inputs are bit-complements so a + b = 0xFFFFFFFFFFFFFFFF; any encoding
  // slip that swaps Rd/Rn/Rm or alters the operation surfaces as a
  // non-saturated result.
  {
    s.current_subtest = "ADD x0, x1, x2";
    alignas(uint32_t) uint32_t code[] = {
        0x8b020020,  // add x0, x1, x2
        0xd65f03c0,  // ret
    };
    const uint64_t a = 0x0123456789abcdefULL;
    const uint64_t b = 0xfedcba9876543210ULL;
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x1, a);
    write_x(sim, x2, b);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s, read_x(sim, x0), a + b, "X0 = X1 + X2");
  }

  // SUB x0, x1, x2 — same encoding shape as ADD but with op-bit set.
  // X1 < X2 forces a 2's-complement wrap; if SUB were mistakenly decoded as
  // RSB or NEG the result would differ sharply, not by a single bit.
  {
    s.current_subtest = "SUB x0, x1, x2 (X1 < X2 → wraps)";
    alignas(uint32_t) uint32_t code[] = {
        0xcb020020,  // sub x0, x1, x2
        0xd65f03c0,  // ret
    };
    const uint64_t a = 7ULL;
    const uint64_t b = 10ULL;
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x1, a);
    write_x(sim, x2, b);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    // a - b in uint64_t modular arithmetic equals AArch64's 64-bit SUB
    // exactly: both wrap modulo 2^64.
    expect_eq_u64(s, read_x(sim, x0), a - b, "X0 = X1 - X2");
  }

  // MUL x0, x1, x2 — MADD with Ra = XZR. Operands chosen so the full 128-bit
  // product exceeds 64 bits; MUL returns only the low 64, which is the part
  // the simulator must compute correctly. Catches a SMULH/UMULH/MNEG mix-up.
  {
    s.current_subtest = "MUL x0, x1, x2 (low 64 of overflowing product)";
    alignas(uint32_t) uint32_t code[] = {
        0x9b027c20,  // mul x0, x1, x2
        0xd65f03c0,  // ret
    };
    const uint64_t a = 0x0000000100000003ULL;
    const uint64_t b = 0x0000000200000005ULL;
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x1, a);
    write_x(sim, x2, b);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s, read_x(sim, x0), a * b, "X0 = (X1 * X2) low 64 bits");
  }
}

void run_logical_tests(TestState& s) {
  s.current_family = "logical";

  // Inputs share the property that AND, ORR, and EOR all produce distinct
  // results, so any encoding swap among the three surfaces as a wrong value
  // rather than coincidentally agreeing.
  //   a = 0xAA…AA (nibble = 1010_b)
  //   b = 0xCC…CC (nibble = 1100_b)
  //   AND = 0x88…88 (1000_b)
  //   ORR = 0xEE…EE (1110_b)
  //   EOR = 0x66…66 (0110_b)
  const uint64_t a = 0xAAAAAAAAAAAAAAAAULL;
  const uint64_t b = 0xCCCCCCCCCCCCCCCCULL;

  // AND x0, x1, x2 — register-register form.
  {
    s.current_subtest = "AND x0, x1, x2";
    alignas(uint32_t) uint32_t code[] = {
        0x8a020020,  // and x0, x1, x2
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x1, a);
    write_x(sim, x2, b);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s, read_x(sim, x0), a & b, "X0 = X1 & X2");
  }

  // ORR x0, x1, x2 — register-register form.
  {
    s.current_subtest = "ORR x0, x1, x2";
    alignas(uint32_t) uint32_t code[] = {
        0xaa020020,  // orr x0, x1, x2
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x1, a);
    write_x(sim, x2, b);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s, read_x(sim, x0), a | b, "X0 = X1 | X2");
  }

  // EOR x0, x1, x2 — register-register form.
  {
    s.current_subtest = "EOR x0, x1, x2";
    alignas(uint32_t) uint32_t code[] = {
        0xca020020,  // eor x0, x1, x2
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x1, a);
    write_x(sim, x2, b);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s, read_x(sim, x0), a ^ b, "X0 = X1 ^ X2");
  }
}

void run_loadstore_tests(TestState& s) {
  s.current_family = "load/store";

  // Host-side fixture per design D6: ordinary stack buffer, addr passed in via
  // X1. The buffer outlives each Simulator since it's at function scope.
  // Note on encoding the immediate: 64-bit LDR/STR scale imm12 by 8, so
  // `#16` is imm12=2 and `#24` is imm12=3 in the encoded fields.
  alignas(uint64_t) std::array<uint8_t, 64> buf{};

  const uint64_t ldr_pattern = 0xCAFEBABEDEADBEEFULL;
  const uint64_t str_value = 0xDEADBEEFFEEDFACEULL;

  // LDR x0, [x1, #16] — immediate (unsigned) offset.
  {
    s.current_subtest = "LDR x0, [x1, #16] (immediate offset)";
    buf.fill(0);
    std::memcpy(buf.data() + 16, &ldr_pattern, sizeof(ldr_pattern));
    alignas(uint32_t) uint32_t code[] = {
        0xf9400820,  // ldr x0, [x1, #16]
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x1, reinterpret_cast<uint64_t>(buf.data()));
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s, read_x(sim, x0), ldr_pattern, "X0 = mem[buf+16]");
  }

  // LDR x0, [x1, x2] — register offset (LSL #0, the default no-shift form).
  {
    s.current_subtest = "LDR x0, [x1, x2] (register offset)";
    buf.fill(0);
    std::memcpy(buf.data() + 16, &ldr_pattern, sizeof(ldr_pattern));
    alignas(uint32_t) uint32_t code[] = {
        0xf8626820,  // ldr x0, [x1, x2]
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x1, reinterpret_cast<uint64_t>(buf.data()));
    write_x(sim, x2, 16ULL);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s, read_x(sim, x0), ldr_pattern, "X0 = mem[buf + X2(16)]");
  }

  // STR x0, [x1, #24] — immediate (unsigned) offset.
  {
    s.current_subtest = "STR x0, [x1, #24] (immediate offset)";
    buf.fill(0);
    alignas(uint32_t) uint32_t code[] = {
        0xf9000c20,  // str x0, [x1, #24]
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x0, str_value);
    write_x(sim, x1, reinterpret_cast<uint64_t>(buf.data()));
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    uint64_t read_back = 0;
    std::memcpy(&read_back, buf.data() + 24, sizeof(read_back));
    expect_eq_u64(s, read_back, str_value, "mem[buf+24] = X0");
  }

  // STR x0, [x1, x2] — register offset (LSL #0).
  {
    s.current_subtest = "STR x0, [x1, x2] (register offset)";
    buf.fill(0);
    alignas(uint32_t) uint32_t code[] = {
        0xf8226820,  // str x0, [x1, x2]
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x0, str_value);
    write_x(sim, x1, reinterpret_cast<uint64_t>(buf.data()));
    write_x(sim, x2, 24ULL);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    uint64_t read_back = 0;
    std::memcpy(&read_back, buf.data() + 24, sizeof(read_back));
    expect_eq_u64(s, read_back, str_value, "mem[buf + X2(24)] = X0");
  }
}

void run_branch_tests(TestState& s) {
  s.current_family = "branch";

  // The "marker" pattern: X0 is preloaded to sentinel_a via WriteXRegister
  // before RunFrom; the encoded MOV inside the sequence overwrites X0 to
  // sentinel_b iff control falls through (i.e., the conditional branch did
  // NOT take). After RET, X0 distinguishes taken (sentinel_a) from
  // not-taken (sentinel_b).
  const uint64_t sentinel_a = 0xA0A0ULL;
  const uint64_t sentinel_b = 0xB0B0ULL;  // encoded as MOVZ X0, #0xB0B0

  // -------- CBZ X1, +8 (taken when X1 == 0) --------
  {
    s.current_subtest = "CBZ X1, +8 — taken (X1 == 0)";
    alignas(uint32_t) uint32_t code[] = {
        0xb4000041,  // cbz x1, #+8       (skip next on Z-reg == 0)
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x0, sentinel_a);
    write_x(sim, x1, 0ULL);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s,
                  read_x(sim, x0),
                  sentinel_a,
                  "X0 == sentinel_a (CBZ taken)");
  }

  {
    s.current_subtest = "CBZ X1, +8 — not-taken (X1 != 0)";
    alignas(uint32_t) uint32_t code[] = {
        0xb4000041,  // cbz x1, #+8
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x0, sentinel_a);
    write_x(sim, x1, 1ULL);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s,
                  read_x(sim, x0),
                  sentinel_b,
                  "X0 == sentinel_b (CBZ not-taken; MOV ran)");
  }

  // -------- CBNZ X1, +8 (taken when X1 != 0) --------
  {
    s.current_subtest = "CBNZ X1, +8 — taken (X1 != 0)";
    alignas(uint32_t) uint32_t code[] = {
        0xb5000041,  // cbnz x1, #+8
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x0, sentinel_a);
    write_x(sim, x1, 1ULL);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s,
                  read_x(sim, x0),
                  sentinel_a,
                  "X0 == sentinel_a (CBNZ taken)");
  }

  {
    s.current_subtest = "CBNZ X1, +8 — not-taken (X1 == 0)";
    alignas(uint32_t) uint32_t code[] = {
        0xb5000041,  // cbnz x1, #+8
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x0, sentinel_a);
    write_x(sim, x1, 0ULL);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s,
                  read_x(sim, x0),
                  sentinel_b,
                  "X0 == sentinel_b (CBNZ not-taken; MOV ran)");
  }

  // -------- B.EQ +8 (taken when NZCV.Z == 1) --------
  // SUBS X3, Xn, Xm precedes the branch per design D8 so the branch outcome
  // depends on observable flag state, not on ResetSystemRegisters defaults.
  // We also assert on sim.ReadZ() per task 1.4 — this separates "wrong flag
  // setter" from "wrong condition decode" in failure diagnostics.

  {
    s.current_subtest = "B.EQ +8 — taken (Z=1 after SUBS X3,X1,X1)";
    alignas(uint32_t) uint32_t code[] = {
        0xeb010023,  // subs x3, x1, x1   ; X1 - X1 = 0 → Z=1
        0x54000040,  // b.eq #+8          (skip MOV when Z=1)
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x0, sentinel_a);
    write_x(sim, x1, 42ULL);  // any value; SUBS X1-X1 produces 0
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s,
                  read_x(sim, x0),
                  sentinel_a,
                  "X0 == sentinel_a (B.EQ taken)");
    expect_eq_u8(s,
                 sim.ReadZ() ? 1 : 0,
                 1,
                 "NZCV.Z == 1 (post SUBS X3, X1, X1)");
  }

  {
    s.current_subtest =
        "B.EQ +8 — not-taken (Z=0 after SUBS X3,X1,X2 with X1!=X2)";
    alignas(uint32_t) uint32_t code[] = {
        0xeb020023,  // subs x3, x1, x2   ; X1 - X2 != 0 → Z=0
        0x54000040,  // b.eq #+8
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    write_x(sim, x0, sentinel_a);
    write_x(sim, x1, 10ULL);
    write_x(sim, x2, 3ULL);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s,
                  read_x(sim, x0),
                  sentinel_b,
                  "X0 == sentinel_b (B.EQ not-taken; MOV ran)");
    expect_eq_u8(s,
                 sim.ReadZ() ? 1 : 0,
                 0,
                 "NZCV.Z == 0 (post SUBS X3, X1, X2 with X1 != X2)");
  }
}

void run_call_return_tests(TestState& s) {
  s.current_family = "call/return";

  // Per design D7: BL clobbers LR with the return address, so the outer LR
  // (== kEndOfSimAddress == nullptr, the terminator) must be preserved
  // across the inner call. We save it to X19 (callee-saved) before the BL
  // and restore it before the outer RET.
  //
  // Sequence layout (PC offsets are byte offsets from sequence start):
  //   PC=0   : mov x19, lr              ; save outer LR (NULL)
  //   PC=4   : bl callee                ; imm26 = +12/4 = 3 → callee at PC=16
  //   PC=8   : mov lr,  x19             ; restore outer LR before outer RET
  //   PC=12  : ret                      ; outer terminator (LR == NULL again)
  //   PC=16  : movz x0, #0xAB1E         ; callee body, low half of sentinel
  //   PC=20  : movk x0, #0xC011, lsl#16 ; callee body, high half
  //   PC=24  : ret                      ; inner RET, returns to PC=8
  //
  // Two legs of the post-state prove both BL and RET worked:
  //   - X0 == 0xC011AB1E: callee executed AND control returned to the
  //     prologue's continuation (so MOV LR, X19 ran, so outer RET found
  //     LR == NULL and terminated cleanly).
  //   - The fact that RunFrom returned at all (we're now executing this
  //     C++): outer RET hit the kEndOfSimAddress terminator. CTest would
  //     time out otherwise.
  {
    s.current_subtest = "BL + RET with outer LR preserved via X19";
    alignas(uint32_t) uint32_t code[] = {
        0xaa1e03f3,  // mov x19, lr             ; orr x19, xzr, x30
        0x94000003,  // bl callee (offset +12)
        0xaa1303fe,  // mov lr,  x19            ; orr x30, xzr, x19
        0xd65f03c0,  // ret                     ; outer
        0xd29563c0,  // movz x0, #0xAB1E
        0xf2b80220,  // movk x0, #0xC011, lsl #16
        0xd65f03c0,  // ret                     ; inner
    };
    Decoder decoder;
    Simulator sim(&decoder, stdout);
    configure_simulator(sim);
    sim.RunFrom(reinterpret_cast<const Instruction*>(&code[0]));
    expect_eq_u64(s,
                  read_x(sim, x0),
                  0xC011AB1EULL,
                  "X0 == sentinel 0xC011AB1E (callee ran AND outer RET "
                  "reached)");
  }
}

}  // namespace

int main() {
  TestState state;

  run_arithmetic_tests(state);
  run_logical_tests(state);
  run_loadstore_tests(state);
  run_branch_tests(state);
  run_call_return_tests(state);

  std::printf("simulator_correctness: %d/%d sub-tests passed\n",
              state.passed,
              state.total);
  return (state.passed == state.total) ? 0 : 1;
}
