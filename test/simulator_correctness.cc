// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// simulator_correctness: dual-path AArch64 leaf-semantics regression test.
//
// Every hand-encoded sequence is run twice through the gaby_vm::Simulator
// public API — once via RunFrom (the predecode-cache track) and once via
// DebugRunFrom (the imported Decoder -> VisitNamedInstruction -> leaf track) —
// and the post-run architectural state is asserted against precomputed
// expected values on each track. Because both tracks are checked against the
// same expectation, a pass also proves the two tracks agree.
//
// This test drives only gaby_vm:: public types, so it links gaby_vm::gaby_vm
// without the privileged build pattern (no PRIVATE src/ include, no VIXL_*
// defines). It supersedes the former single-path test that called
// vixl::aarch64::Simulator::RunFrom directly.
//
// Why hand-encoded uint32_t literals (no assembler).
// --------------------------------------------------
// The imported VIXL tree is bounded to Tiers 1+2+3 of the extraction map; the
// assembler (Tier 0) is deliberately not imported. Each instruction word below
// is therefore a literal hex constant with the AArch64 mnemonic in a comment.
// Hand-typed encodings are easy to get wrong, so every encoding is verified at
// authorship time with an external assembler before commit:
//
//     printf 'add x0, x1, x2\nret\n' \
//         | clang -target aarch64-linux-gnu -c -x assembler -o /tmp/enc.o -
//     objdump -d /tmp/enc.o
//
// These tools are never invoked by the build or test — the runtime binary
// contains only uint32_t literals.
//
// Termination protocol.
// ---------------------
// gaby_vm::Simulator's underlying state initializes the link register to the
// end-of-simulation sentinel (a null LR). Every encoded sequence ends with RET
// (0xd65f03c0): the implicit null LR set up at construction is the termination
// contract. The BL+RET sub-test preserves the outer LR across the inner call
// so the outer RET still terminates the run.
// =============================================================================

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/simulator.h"

namespace {

// -------- Test state --------

struct TestState {
  int passed = 0;
  int total = 0;
  const char* family = "";
  const char* subtest = "";
  const char* track = "";
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
               "[FAIL] %s / %s / %s track: %s\n"
               "  actual   = 0x%016llx (%llu)\n"
               "  expected = 0x%016llx (%llu)\n",
               s.family,
               s.subtest,
               s.track,
               label,
               static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(expected),
               static_cast<unsigned long long>(expected));
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
               "[FAIL] %s / %s / %s track: %s\n"
               "  actual   = 0x%02x\n"
               "  expected = 0x%02x\n",
               s.family,
               s.subtest,
               s.track,
               label,
               actual,
               expected);
  return false;
}

// AArch64 NZCV.Z bit (bit 30 of the architectural flag layout).
uint8_t nzcv_z(const gaby_vm::Simulator& sim) {
  return static_cast<uint8_t>((sim.Read(gaby_vm::SysRegister::NZCV) >> 30) &
                              1u);
}

// -------- Embedder-owned guest stack --------

struct StackBuffer {
  alignas(16) std::array<uint8_t, 16 * 1024> bytes{};
  void* data() { return bytes.data(); }
  size_t size() const { return bytes.size(); }
};

// -------- Dual-path runner --------
//
// Runs `code` through both tracks. `setup` writes the initial register (and
// fixture) state and is re-applied before each track, so the two runs start
// identically. `verify` reads post-run state and asserts it; it runs once per
// track, with s.track naming which.

using SetupFn = std::function<void(gaby_vm::Simulator&)>;
using VerifyFn = std::function<void(gaby_vm::Simulator&)>;

void run_dual(TestState& s,
              const char* subtest,
              const uint32_t* code,
              size_t code_words,
              const SetupFn& setup,
              const VerifyFn& verify) {
  s.subtest = subtest;
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  // Cache track: register the sequence, then RunFrom.
  {
    s.track = "cache";
    gaby_vm::PredecodeCache cache;
    gaby_vm::PredecodeCache::RegistrationStatus status =
        cache.RegisterCodeRange(code, code_words * sizeof(uint32_t));
    if (status != gaby_vm::PredecodeCache::RegistrationStatus::Ok) {
      s.total += 1;
      std::fprintf(stderr,
                   "[FAIL] %s / %s / cache track: RegisterCodeRange failed\n",
                   s.family,
                   s.subtest);
    } else {
      StackBuffer stack;
      gaby_vm::Simulator sim(&cache, stack.data(), stack.size());
      setup(sim);
      sim.RunFrom(entry);
      verify(sim);
    }
  }

  // Debug track: a null-cache Simulator drives the imported decoder flow.
  {
    s.track = "debug";
    StackBuffer stack;
    gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());
    setup(sim);
    sim.DebugRunFrom(entry);
    verify(sim);
  }
}

// -------- Integer arithmetic: ADD, SUB, MUL --------

void run_arithmetic_tests(TestState& s) {
  s.family = "arithmetic";

  // ADD x0, x1, x2 — register form. Bit-complement inputs sum to all-ones.
  {
    const uint64_t a = 0x0123456789abcdefULL;
    const uint64_t b = 0xfedcba9876543210ULL;
    uint32_t code[] = {
        0x8b020020,  // add x0, x1, x2
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "ADD x0, x1, x2",
        code,
        2,
        [=](gaby_vm::Simulator& sim) {
          sim.Write(gaby_vm::GpRegister::X1, a);
          sim.Write(gaby_vm::GpRegister::X2, b);
        },
        [=, &s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        a + b,
                        "X0 = X1 + X2");
        });
  }

  // SUB x0, x1, x2 — X1 < X2 forces a 2's-complement wrap.
  {
    const uint64_t a = 7ULL;
    const uint64_t b = 10ULL;
    uint32_t code[] = {
        0xcb020020,  // sub x0, x1, x2
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "SUB x0, x1, x2 (X1 < X2 wraps)",
        code,
        2,
        [=](gaby_vm::Simulator& sim) {
          sim.Write(gaby_vm::GpRegister::X1, a);
          sim.Write(gaby_vm::GpRegister::X2, b);
        },
        [=, &s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        a - b,
                        "X0 = X1 - X2");
        });
  }

  // MUL x0, x1, x2 — low 64 bits of an overflowing product.
  {
    const uint64_t a = 0x0000000100000003ULL;
    const uint64_t b = 0x0000000200000005ULL;
    uint32_t code[] = {
        0x9b027c20,  // mul x0, x1, x2
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "MUL x0, x1, x2 (low 64 of product)",
        code,
        2,
        [=](gaby_vm::Simulator& sim) {
          sim.Write(gaby_vm::GpRegister::X1, a);
          sim.Write(gaby_vm::GpRegister::X2, b);
        },
        [=, &s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        a * b,
                        "X0 = (X1 * X2) low 64 bits");
        });
  }
}

// -------- Logical: AND, ORR, EOR --------

void run_logical_tests(TestState& s) {
  s.family = "logical";

  // Inputs chosen so AND, ORR, EOR all give distinct results.
  const uint64_t a = 0xAAAAAAAAAAAAAAAAULL;
  const uint64_t b = 0xCCCCCCCCCCCCCCCCULL;
  const SetupFn setup = [=](gaby_vm::Simulator& sim) {
    sim.Write(gaby_vm::GpRegister::X1, a);
    sim.Write(gaby_vm::GpRegister::X2, b);
  };

  {
    uint32_t code[] = {
        0x8a020020,  // and x0, x1, x2
        0xd65f03c0,  // ret
    };
    run_dual(s,
             "AND x0, x1, x2",
             code,
             2,
             setup,
             [=, &s](gaby_vm::Simulator& sim) {
               expect_eq_u64(s,
                             sim.Read(gaby_vm::GpRegister::X0),
                             a & b,
                             "X0 = X1 & X2");
             });
  }
  {
    uint32_t code[] = {
        0xaa020020,  // orr x0, x1, x2
        0xd65f03c0,  // ret
    };
    run_dual(s,
             "ORR x0, x1, x2",
             code,
             2,
             setup,
             [=, &s](gaby_vm::Simulator& sim) {
               expect_eq_u64(s,
                             sim.Read(gaby_vm::GpRegister::X0),
                             a | b,
                             "X0 = X1 | X2");
             });
  }
  {
    uint32_t code[] = {
        0xca020020,  // eor x0, x1, x2
        0xd65f03c0,  // ret
    };
    run_dual(s,
             "EOR x0, x1, x2",
             code,
             2,
             setup,
             [=, &s](gaby_vm::Simulator& sim) {
               expect_eq_u64(s,
                             sim.Read(gaby_vm::GpRegister::X0),
                             a ^ b,
                             "X0 = X1 ^ X2");
             });
  }
}

// -------- Load / store --------

void run_loadstore_tests(TestState& s) {
  s.family = "load/store";

  // Host-side fixture: an ordinary stack buffer, its address passed in via X1.
  // setup re-zeroes it before each track so the two runs are independent.
  alignas(uint64_t) std::array<uint8_t, 64> buf{};
  const uint64_t ldr_pattern = 0xCAFEBABEDEADBEEFULL;
  const uint64_t str_value = 0xDEADBEEFFEEDFACEULL;

  // LDR x0, [x1, #16] — immediate (unsigned) offset.
  {
    uint32_t code[] = {
        0xf9400820,  // ldr x0, [x1, #16]
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "LDR x0, [x1, #16]",
        code,
        2,
        [&](gaby_vm::Simulator& sim) {
          buf.fill(0);
          std::memcpy(buf.data() + 16, &ldr_pattern, sizeof(ldr_pattern));
          sim.Write(gaby_vm::GpRegister::X1,
                    reinterpret_cast<uint64_t>(buf.data()));
        },
        [&](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        ldr_pattern,
                        "X0 = mem[buf+16]");
        });
  }

  // LDR x0, [x1, x2] — register offset.
  {
    uint32_t code[] = {
        0xf8626820,  // ldr x0, [x1, x2]
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "LDR x0, [x1, x2]",
        code,
        2,
        [&](gaby_vm::Simulator& sim) {
          buf.fill(0);
          std::memcpy(buf.data() + 16, &ldr_pattern, sizeof(ldr_pattern));
          sim.Write(gaby_vm::GpRegister::X1,
                    reinterpret_cast<uint64_t>(buf.data()));
          sim.Write(gaby_vm::GpRegister::X2, 16ULL);
        },
        [&](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        ldr_pattern,
                        "X0 = mem[buf + X2(16)]");
        });
  }

  // STR x0, [x1, #24] — immediate (unsigned) offset.
  {
    uint32_t code[] = {
        0xf9000c20,  // str x0, [x1, #24]
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "STR x0, [x1, #24]",
        code,
        2,
        [&](gaby_vm::Simulator& sim) {
          buf.fill(0);
          sim.Write(gaby_vm::GpRegister::X0, str_value);
          sim.Write(gaby_vm::GpRegister::X1,
                    reinterpret_cast<uint64_t>(buf.data()));
        },
        [&](gaby_vm::Simulator& /*sim*/) {
          uint64_t read_back = 0;
          std::memcpy(&read_back, buf.data() + 24, sizeof(read_back));
          expect_eq_u64(s, read_back, str_value, "mem[buf+24] = X0");
        });
  }

  // STR x0, [x1, x2] — register offset.
  {
    uint32_t code[] = {
        0xf8226820,  // str x0, [x1, x2]
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "STR x0, [x1, x2]",
        code,
        2,
        [&](gaby_vm::Simulator& sim) {
          buf.fill(0);
          sim.Write(gaby_vm::GpRegister::X0, str_value);
          sim.Write(gaby_vm::GpRegister::X1,
                    reinterpret_cast<uint64_t>(buf.data()));
          sim.Write(gaby_vm::GpRegister::X2, 24ULL);
        },
        [&](gaby_vm::Simulator& /*sim*/) {
          uint64_t read_back = 0;
          std::memcpy(&read_back, buf.data() + 24, sizeof(read_back));
          expect_eq_u64(s, read_back, str_value, "mem[buf + X2(24)] = X0");
        });
  }
}

// -------- Conditional control flow --------

void run_branch_tests(TestState& s) {
  s.family = "branch";

  // X0 is preloaded to sentinel_a; the encoded MOV overwrites it to sentinel_b
  // iff control falls through (the conditional branch did NOT take).
  const uint64_t sentinel_a = 0xA0A0ULL;
  const uint64_t sentinel_b = 0xB0B0ULL;  // MOVZ X0, #0xB0B0

  // CBZ X1, +8 — taken when X1 == 0.
  {
    uint32_t code[] = {
        0xb4000041,  // cbz x1, #+8
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "CBZ X1, +8 — taken (X1 == 0)",
        code,
        3,
        [=](gaby_vm::Simulator& sim) {
          sim.Write(gaby_vm::GpRegister::X0, sentinel_a);
          sim.Write(gaby_vm::GpRegister::X1, 0ULL);
        },
        [=, &s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        sentinel_a,
                        "X0 == sentinel_a (CBZ taken)");
        });
    run_dual(
        s,
        "CBZ X1, +8 — not-taken (X1 != 0)",
        code,
        3,
        [=](gaby_vm::Simulator& sim) {
          sim.Write(gaby_vm::GpRegister::X0, sentinel_a);
          sim.Write(gaby_vm::GpRegister::X1, 1ULL);
        },
        [=, &s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        sentinel_b,
                        "X0 == sentinel_b (CBZ not-taken; MOV ran)");
        });
  }

  // CBNZ X1, +8 — taken when X1 != 0.
  {
    uint32_t code[] = {
        0xb5000041,  // cbnz x1, #+8
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "CBNZ X1, +8 — taken (X1 != 0)",
        code,
        3,
        [=](gaby_vm::Simulator& sim) {
          sim.Write(gaby_vm::GpRegister::X0, sentinel_a);
          sim.Write(gaby_vm::GpRegister::X1, 1ULL);
        },
        [=, &s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        sentinel_a,
                        "X0 == sentinel_a (CBNZ taken)");
        });
    run_dual(
        s,
        "CBNZ X1, +8 — not-taken (X1 == 0)",
        code,
        3,
        [=](gaby_vm::Simulator& sim) {
          sim.Write(gaby_vm::GpRegister::X0, sentinel_a);
          sim.Write(gaby_vm::GpRegister::X1, 0ULL);
        },
        [=, &s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        sentinel_b,
                        "X0 == sentinel_b (CBNZ not-taken; MOV ran)");
        });
  }

  // B.EQ +8 after SUBS — the branch outcome depends on observable flag state.
  {
    uint32_t code[] = {
        0xeb010023,  // subs x3, x1, x1   ; X1 - X1 = 0 -> Z=1
        0x54000040,  // b.eq #+8
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "B.EQ +8 — taken (Z=1 after SUBS X3,X1,X1)",
        code,
        4,
        [=](gaby_vm::Simulator& sim) {
          sim.Write(gaby_vm::GpRegister::X0, sentinel_a);
          sim.Write(gaby_vm::GpRegister::X1, 42ULL);
        },
        [=, &s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        sentinel_a,
                        "X0 == sentinel_a (B.EQ taken)");
          expect_eq_u8(s, nzcv_z(sim), 1, "NZCV.Z == 1 (post SUBS X1,X1)");
        });
  }
  {
    uint32_t code[] = {
        0xeb020023,  // subs x3, x1, x2   ; X1 - X2 != 0 -> Z=0
        0x54000040,  // b.eq #+8
        0xd2961600,  // mov x0, #0xB0B0
        0xd65f03c0,  // ret
    };
    run_dual(
        s,
        "B.EQ +8 — not-taken (Z=0 after SUBS X3,X1,X2)",
        code,
        4,
        [=](gaby_vm::Simulator& sim) {
          sim.Write(gaby_vm::GpRegister::X0, sentinel_a);
          sim.Write(gaby_vm::GpRegister::X1, 10ULL);
          sim.Write(gaby_vm::GpRegister::X2, 3ULL);
        },
        [=, &s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        sentinel_b,
                        "X0 == sentinel_b (B.EQ not-taken; MOV ran)");
          expect_eq_u8(s, nzcv_z(sim), 0, "NZCV.Z == 0 (post SUBS X1,X2)");
        });
  }
}

// -------- Procedure call / return --------

void run_call_return_tests(TestState& s) {
  s.family = "call/return";

  // BL clobbers LR with the return address, so the outer LR (the null-LR
  // terminator) is saved to X19 before the BL and restored before the outer
  // RET. Layout (byte offsets from sequence start):
  //   0  : mov x19, lr               ; save outer LR (null)
  //   4  : bl callee                 ; imm26 = +12/4 -> callee at offset 16
  //   8  : mov lr,  x19              ; restore outer LR
  //   12 : ret                       ; outer terminator
  //   16 : movz x0, #0xAB1E          ; callee body, low half
  //   20 : movk x0, #0xC011, lsl#16  ; callee body, high half
  //   24 : ret                       ; inner RET, returns to offset 8
  {
    uint32_t code[] = {
        0xaa1e03f3,  // mov x19, lr
        0x94000003,  // bl callee (+12)
        0xaa1303fe,  // mov lr,  x19
        0xd65f03c0,  // ret  (outer)
        0xd29563c0,  // movz x0, #0xAB1E
        0xf2b80220,  // movk x0, #0xC011, lsl #16
        0xd65f03c0,  // ret  (inner)
    };
    run_dual(
        s,
        "BL + RET with outer LR preserved via X19",
        code,
        7,
        [](gaby_vm::Simulator& /*sim*/) {},
        [&s](gaby_vm::Simulator& sim) {
          expect_eq_u64(s,
                        sim.Read(gaby_vm::GpRegister::X0),
                        0xC011AB1EULL,
                        "X0 == 0xC011AB1E (callee ran AND outer RET reached)");
        });
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

  std::printf("simulator_correctness: %d/%d dual-path checks passed\n",
              state.passed,
              state.total);
  return (state.passed == state.total) ? 0 : 1;
}
