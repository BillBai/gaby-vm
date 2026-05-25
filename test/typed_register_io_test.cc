// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

// =============================================================================
// typed_register_io_test: gaby_vm::Simulator typed register-I/O surface.
//
// Drives the change introduced by the simulator-register-io-api OpenSpec
// change. Covers, in order:
//
//   - Per-register round-trip on the typed Read/Write surface for GP, V,
//     and Sys registers.
//   - SP / XZR disambiguation under GpRegister::SP.
//   - PC seating: Write(GpRegister::PC, …) followed by StepOnce executes the
//     instruction at the supplied entry point.
//   - System-register round-trip for NZCV / FPCR / FPSR / BType (FPSR is the
//     interesting one — VIXL doesn't model it, so the round-trip relies on
//     the Pimpl-side storage slot the change added).
//   - ReadAll → execute → WriteAll → ReadAll round-trip.
//   - WriteAll with distinct sentinel values in every slot.
//   - Mixed std::span<RegisterWrite> batch write.
//   - Compile-time RegisterFile properties (size, standard-layout,
//     trivially copyable).
//
// Public-API only: no privileged VIXL build pattern, no imported headers in
// scope. Hand-encoded instruction words (verified at authorship time with an
// external assembler, per docs/testing.md).
// =============================================================================

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <type_traits>
#include <variant>

#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

namespace {

// Compile-time RegisterFile contract (task 5.9). Failing any of these is a
// build error, not a runtime check — which is the point.
static_assert(sizeof(gaby_vm::RegisterFile) == 31 * 8 + 8 + 8 + 32 * 16 + 4 * 4,
              "RegisterFile size has drifted from the spec");
static_assert(std::is_standard_layout_v<gaby_vm::RegisterFile>,
              "RegisterFile must be standard-layout");
static_assert(std::is_trivially_copyable_v<gaby_vm::RegisterFile>,
              "RegisterFile must be trivially copyable");
static_assert(
    std::is_same_v<
        gaby_vm::RegisterWrite,
        std::variant<gaby_vm::GpWrite, gaby_vm::VWrite, gaby_vm::SysWrite>>,
    "RegisterWrite must resolve to the documented variant");

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

struct StackBuffer {
  alignas(16) std::array<uint8_t, 16 * 1024> bytes{};
  void* data() { return bytes.data(); }
  size_t size() const { return bytes.size(); }
};

// AArch64 encodings (verified with an external assembler at authorship time).
constexpr uint32_t kAddX0X0_1 = 0x91000400u;  // add x0, x0, #1
constexpr uint32_t kRet = 0xd65f03c0u;        // ret              (ret x30)

// --- Task 5.1: per-register typed Read/Write round-trip ---------------------

void test_gp_register_round_trip() {
  StackBuffer stack;
  gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());

  // X0..X30 each round-trip independently.
  for (unsigned i = 0; i <= 30; ++i) {
    const gaby_vm::GpRegister reg = static_cast<gaby_vm::GpRegister>(i);
    const uint64_t value = 0xdeadbeef00000000ull | (i + 1);
    sim.Write(reg, value);
    char label[48];
    std::snprintf(label, sizeof(label), "GP X%u write/read round-trip", i);
    check(sim.Read(reg) == value, label);
  }
}

void test_v_register_round_trip() {
  StackBuffer stack;
  gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());

  for (unsigned i = 0; i <= 31; ++i) {
    const gaby_vm::VRegister reg = static_cast<gaby_vm::VRegister>(i);
    const gaby_vm::VRegisterValue value{0x1111111122222222ull + i,
                                        0x3333333344444444ull + i};
    sim.Write(reg, value);
    const gaby_vm::VRegisterValue read = sim.Read(reg);
    char label[64];
    std::snprintf(label,
                  sizeof(label),
                  "V%u write/read round-trip preserves lo and hi",
                  i);
    check(read.lo == value.lo && read.hi == value.hi, label);
  }
}

// --- Task 5.2: SP / XZR disambiguation --------------------------------------
//
// Writing GpRegister::SP must land in the dedicated SP slot, not the
// XZR-encoded "X31" path. The typed surface offers no GpRegister::XZR, so we
// exercise this by writing distinct values to GP X30 and SP and confirming
// they're independent: a GP register write must not clobber SP and vice
// versa. The SP-as-stack invariant is also indirectly checked by the
// existing simulator_correctness tests, which exercise LDR/STR via SP.

void test_sp_vs_xzr_disambiguation() {
  StackBuffer stack;
  gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());

  sim.Write(gaby_vm::GpRegister::SP, 0xfeedfacedeadbeefull);
  sim.Write(gaby_vm::GpRegister::X30,
            0x1122334455667788ull);  // adjacent slot

  check(sim.Read(gaby_vm::GpRegister::SP) == 0xfeedfacedeadbeefull,
        "SP write reaches the SP slot");
  check(sim.Read(gaby_vm::GpRegister::X30) == 0x1122334455667788ull,
        "X30 write is independent of SP");
}

// --- Task 5.3: PC seating + StepOnce ----------------------------------------

void test_pc_seating_executes_at_entry() {
  alignas(uint32_t) uint32_t code[] = {kAddX0X0_1, kRet};
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  gaby_vm::PredecodeCache cache;
  check(cache.RegisterCodeRange(code, sizeof(code)) ==
            gaby_vm::PredecodeCache::RegistrationStatus::Ok,
        "PC-seating: code range registers Ok");

  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  // Seat X0 = 41 and PC = &kAddX0X0_1. After one step, X0 should be 42.
  sim.Write(gaby_vm::GpRegister::X0, 41);
  sim.Write(gaby_vm::GpRegister::PC, entry);

  const bool alive = sim.StepOnce();
  check(alive, "PC-seating: StepOnce reports the simulation is still alive");
  check(sim.Read(gaby_vm::GpRegister::X0) == 42,
        "PC-seating: ADD X0,X0,#1 executed at the seated PC");
}

// --- Task 5.4: per-sysreg typed Read/Write round-trip -----------------------

void test_sysreg_round_trips() {
  StackBuffer stack;
  gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());

  // NZCV — write to the top 4 bits (which the architecture allows). 0xa0000000
  // is N=1, Z=0, C=1, V=0.
  sim.Write(gaby_vm::SysRegister::NZCV, 0xa0000000u);
  check(sim.Read(gaby_vm::SysRegister::NZCV) == 0xa0000000u,
        "NZCV write/read round-trip");

  // FPCR — the architecture permits writes to the DN bit (bit 25). Read the
  // current value, toggle DN, write it back, and confirm only DN changed.
  const uint32_t fpcr_before = sim.Read(gaby_vm::SysRegister::FPCR);
  const uint32_t fpcr_target = fpcr_before ^ (1u << 25);
  sim.Write(gaby_vm::SysRegister::FPCR, fpcr_target);
  check(sim.Read(gaby_vm::SysRegister::FPCR) == fpcr_target,
        "FPCR write/read round-trip with DN bit toggled");

  // FPSR — VIXL does not model FPSR; the typed surface promises a round-trip
  // via the Pimpl's own storage slot. The whole 32-bit word round-trips.
  sim.Write(gaby_vm::SysRegister::FPSR, 0xcafef00du);
  check(sim.Read(gaby_vm::SysRegister::FPSR) == 0xcafef00du,
        "FPSR write/read round-trip uses Pimpl-side storage");

  // BType — values in [0, 3]. 2 = CallBType.
  sim.Write(gaby_vm::SysRegister::BType, 2u);
  check(sim.Read(gaby_vm::SysRegister::BType) == 2u,
        "BType write/read round-trip");
}

// --- Task 5.5: ReadAll → execute → WriteAll → ReadAll round-trip ------------

void test_read_all_write_all_round_trip() {
  alignas(uint32_t) uint32_t code[] = {kAddX0X0_1, kRet};
  const uintptr_t entry = reinterpret_cast<uintptr_t>(code);

  gaby_vm::PredecodeCache cache;
  check(cache.RegisterCodeRange(code, sizeof(code)) ==
            gaby_vm::PredecodeCache::RegistrationStatus::Ok,
        "ReadAll/WriteAll: code range registers Ok");

  StackBuffer stack;
  gaby_vm::Simulator sim(&cache, stack.data(), stack.size());

  // Seed X0 so the ADD has something to modify, then snapshot.
  sim.Write(gaby_vm::GpRegister::X0, 100);
  gaby_vm::RegisterFile snap = sim.ReadAll();

  // Execute one instruction. X0 changes (100 -> 101), PC advances.
  sim.Write(gaby_vm::GpRegister::PC, entry);
  sim.StepOnce();
  check(sim.Read(gaby_vm::GpRegister::X0) == 101,
        "ReadAll/WriteAll: ADD modified X0 mid-test");

  // Restore the snapshot. After WriteAll, every slot equals the snapshot's
  // field. ReadAll returns a struct equal to snap byte-for-byte.
  sim.WriteAll(snap);
  const gaby_vm::RegisterFile after = sim.ReadAll();
  check(std::memcmp(&after, &snap, sizeof(snap)) == 0,
        "ReadAll/WriteAll: round-trip restores byte-equal state");
}

// --- Task 5.6: WriteAll coverage with distinct sentinels --------------------

void test_write_all_covers_every_slot() {
  StackBuffer stack;
  gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());

  gaby_vm::RegisterFile file{};
  for (unsigned i = 0; i <= 30; ++i) {
    file.x[i] = 0xa000000000000000ull + i;
  }
  file.sp = 0xb000000000000000ull;
  // PC must stay in the simulator's untagged-address range — VIXL's WritePc
  // strips PAC-style tag bits from the top of the address. Anything with the
  // top byte set would readback as zero, so use a value safely below.
  file.pc = 0x0000000000400000ull;
  for (unsigned i = 0; i <= 31; ++i) {
    file.v[i].lo = 0xd000000000000000ull + i;
    file.v[i].hi = 0xe000000000000000ull + i;
  }
  file.nzcv = 0x50000000u;  // C=1, V=1 (writable top-4 bits)
  file.fpcr = sim.Read(gaby_vm::SysRegister::FPCR) | (1u << 25);  // DN bit
  file.fpsr = 0x12345678u;  // FPSR is unmodeled storage; any value round-trips
  file.btype = 1u;          // JumpBType

  sim.WriteAll(file);

  for (unsigned i = 0; i <= 30; ++i) {
    const gaby_vm::GpRegister reg = static_cast<gaby_vm::GpRegister>(i);
    char label[48];
    std::snprintf(label, sizeof(label), "WriteAll: X%u sentinel readback", i);
    check(sim.Read(reg) == file.x[i], label);
  }
  check(sim.Read(gaby_vm::GpRegister::SP) == file.sp, "WriteAll: SP readback");
  check(sim.Read(gaby_vm::GpRegister::PC) == file.pc, "WriteAll: PC readback");
  for (unsigned i = 0; i <= 31; ++i) {
    const gaby_vm::VRegister reg = static_cast<gaby_vm::VRegister>(i);
    const gaby_vm::VRegisterValue v = sim.Read(reg);
    char label[48];
    std::snprintf(label, sizeof(label), "WriteAll: V%u sentinel readback", i);
    check(v.lo == file.v[i].lo && v.hi == file.v[i].hi, label);
  }
  check(sim.Read(gaby_vm::SysRegister::NZCV) == file.nzcv,
        "WriteAll: NZCV readback");
  check(sim.Read(gaby_vm::SysRegister::FPCR) == file.fpcr,
        "WriteAll: FPCR readback");
  check(sim.Read(gaby_vm::SysRegister::FPSR) == file.fpsr,
        "WriteAll: FPSR readback");
  check(sim.Read(gaby_vm::SysRegister::BType) == file.btype,
        "WriteAll: BType readback");
}

// --- Task 5.7: mixed std::span<RegisterWrite> batch -------------------------

void test_span_batch_write_mixed() {
  StackBuffer stack;
  gaby_vm::Simulator sim(nullptr, stack.data(), stack.size());

  const std::array<gaby_vm::RegisterWrite, 3> writes =
      {gaby_vm::RegisterWrite{gaby_vm::GpWrite{gaby_vm::GpRegister::X0, 0x42}},
       gaby_vm::RegisterWrite{
           gaby_vm::VWrite{gaby_vm::VRegister::V0,
                           gaby_vm::VRegisterValue{0xaaaa, 0xbbbb}}},
       gaby_vm::RegisterWrite{
           gaby_vm::SysWrite{gaby_vm::SysRegister::NZCV, 0x40000000u}}};
  sim.Write(std::span<const gaby_vm::RegisterWrite>(writes));

  check(sim.Read(gaby_vm::GpRegister::X0) == 0x42,
        "span batch: GpWrite element applied");
  const gaby_vm::VRegisterValue v = sim.Read(gaby_vm::VRegister::V0);
  check(v.lo == 0xaaaa && v.hi == 0xbbbb,
        "span batch: VWrite element applied with both halves");
  check(sim.Read(gaby_vm::SysRegister::NZCV) == 0x40000000u,
        "span batch: SysWrite element applied");
}

}  // namespace

int main() {
  test_gp_register_round_trip();
  test_v_register_round_trip();
  test_sp_vs_xzr_disambiguation();
  test_pc_seating_executes_at_entry();
  test_sysreg_round_trips();
  test_read_all_write_all_round_trip();
  test_write_all_covers_every_slot();
  test_span_batch_write_mixed();

  std::printf("typed_register_io: %d/%d checks passed\n", g_passed, g_total);
  return (g_passed == g_total) ? 0 : 1;
}
