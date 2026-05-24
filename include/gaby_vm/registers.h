// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef GABY_VM_REGISTERS_H_
#define GABY_VM_REGISTERS_H_

#include <cstdint>
#include <type_traits>
#include <variant>

// gaby_vm::registers — strongly-typed register identifiers and FFI-stable
// register-data types for the gaby_vm::Simulator typed register-I/O surface.
//
// This header is part of the gaby-vm public API. It exposes no vixl:: type and
// includes no imported VIXL header, so it is safe to include from embedder
// code, FFI bindings, or other public headers without dragging the imported
// simulator into scope.
//
// Most of what lives here exists for one of two reasons:
//
//   * Strong typing at the call site — GpRegister / VRegister / SysRegister.
//     A V-register code cannot reach an X-register accessor (or vice versa)
//     because the parameter types differ; the underlying integer values are
//     an internal dispatch detail and are not promised to match VIXL codes.
//
//   * FFI handoff — RegisterFile is a flat POD that mirrors the full guest
//     architectural state, sized and laid out so a C / Swift consumer can
//     read or stamp one directly. Its layout is frozen by a static_assert.
//
// The std::span partial-write surface (Simulator::Write(std::span<...>) with
// RegisterWrite = std::variant<...>) is a C++ ergonomic convenience layer over
// the same individual typed Write entry points; it is NOT ABI-stable across
// libc++/libstdc++ versions and is intentionally C++-only. FFI consumers drive
// partial writes through individual typed Write calls or restore full state
// through WriteAll(RegisterFile).

namespace gaby_vm {

// Strongly-typed AArch64 general-purpose register identifier. Covers X0..X30,
// an LR alias for X30, the dedicated SP slot, and the program counter.
// `enum class : uint8_t` fixes the underlying width at one byte for FFI use.
enum class GpRegister : uint8_t {
  X0 = 0,
  X1 = 1,
  X2 = 2,
  X3 = 3,
  X4 = 4,
  X5 = 5,
  X6 = 6,
  X7 = 7,
  X8 = 8,
  X9 = 9,
  X10 = 10,
  X11 = 11,
  X12 = 12,
  X13 = 13,
  X14 = 14,
  X15 = 15,
  X16 = 16,
  X17 = 17,
  X18 = 18,
  X19 = 19,
  X20 = 20,
  X21 = 21,
  X22 = 22,
  X23 = 23,
  X24 = 24,
  X25 = 25,
  X26 = 26,
  X27 = 27,
  X28 = 28,
  X29 = 29,
  X30 = 30,
  LR = X30,  // X30 carries the link register; LR is the same slot.
  SP = 31,   // Stack pointer. Distinct from the XZR encoding of code 31.
  PC = 32,   // Program counter.
};

// Strongly-typed AArch64 FP/SIMD register identifier covering V0..V31.
enum class VRegister : uint8_t {
  V0 = 0,
  V1 = 1,
  V2 = 2,
  V3 = 3,
  V4 = 4,
  V5 = 5,
  V6 = 6,
  V7 = 7,
  V8 = 8,
  V9 = 9,
  V10 = 10,
  V11 = 11,
  V12 = 12,
  V13 = 13,
  V14 = 14,
  V15 = 15,
  V16 = 16,
  V17 = 17,
  V18 = 18,
  V19 = 19,
  V20 = 20,
  V21 = 21,
  V22 = 22,
  V23 = 23,
  V24 = 24,
  V25 = 25,
  V26 = 26,
  V27 = 27,
  V28 = 28,
  V29 = 29,
  V30 = 30,
  V31 = 31,
};

// Strongly-typed identifier for the simulator's user-visible system registers.
enum class SysRegister : uint8_t {
  NZCV = 0,   // Condition flags (N, Z, C, V) in the architectural layout.
  FPCR = 1,   // Floating-point control register.
  FPSR = 2,   // Floating-point status register.
  BType = 3,  // Branch-type tracking register (BTI).
};

// The full 128-bit value of one FP/SIMD (V) register, split into two 64-bit
// halves. Used as the value type of every V-register accessor — single,
// bulk, and span — so callers can compare V registers over their entire
// width.
struct VRegisterValue {
  uint64_t lo;  // bits [63:0]
  uint64_t hi;  // bits [127:64]
};

// Flat POD snapshot of the full guest architectural state. Field order, types,
// and total size are spec-frozen (see static_asserts below) so that a binding
// generator or hand-written FFI shim can mirror this layout in C / Swift /
// other consumers without needing to track structure changes byte-by-byte.
//
// `pc` is stored as `uint64_t` rather than `uintptr_t`: the project targets
// 64-bit hosts (the two are the same width there), and using a fixed integer
// keeps the layout host-pointer-width-independent.
struct RegisterFile {
  uint64_t x[31];        // X0..X30
  uint64_t sp;           // Stack pointer.
  uint64_t pc;           // Program counter, as a host address.
  VRegisterValue v[32];  // V0..V31, each 128 bits.
  uint32_t nzcv;
  uint32_t fpcr;
  uint32_t fpsr;
  uint32_t btype;
};

// Layout freeze: any drift in the struct above breaks this assert, forcing a
// deliberate re-check of the FFI surface and any binding-side mirrors.
static_assert(sizeof(RegisterFile) == 31 * 8 + 8 + 8 + 32 * 16 + 4 * 4,
              "RegisterFile layout has drifted; update the struct, this "
              "assert, and any FFI bindings together.");
static_assert(std::is_standard_layout_v<RegisterFile>,
              "RegisterFile must be standard-layout so FFI consumers can "
              "mirror its field offsets.");
static_assert(std::is_trivially_copyable_v<RegisterFile>,
              "RegisterFile must be trivially copyable so it can be passed "
              "by value across the FFI boundary.");

// Typed-write payload structs for the partial-batch `Write(std::span<...>)`
// surface. Each carries an enum-typed register identifier and the matching
// value type, so kind-vs-payload mismatches are not even representable.
struct GpWrite {
  GpRegister reg;
  uint64_t value;
};

struct VWrite {
  VRegister reg;
  VRegisterValue value;
};

struct SysWrite {
  SysRegister reg;
  uint32_t value;
};

// Sum type over the three typed-write payloads. Construct via the variant's
// converting constructor (e.g. `RegisterWrite{GpWrite{GpRegister::X3, 42}}`);
// dispatch on `std::visit` inside the simulator's span-write implementation.
//
// `std::variant`'s storage layout is implementation-defined, so this type is
// intentionally C++-only — see registers.h's top-level note.
using RegisterWrite = std::variant<GpWrite, VWrite, SysWrite>;

}  // namespace gaby_vm

#endif  // GABY_VM_REGISTERS_H_
