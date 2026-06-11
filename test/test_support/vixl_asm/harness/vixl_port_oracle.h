// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Oracle for the live-assemble vixl_port harness. Ported from the frozen
// vixl_port_runner.cc (SeatEntry / CheckAssert / DifferentialEqual), but
// reading from RegisterFile snapshots taken after each track runs rather than
// from a live Simulator, and checking against the VIXL reference sim's
// body-exit values (the absolute oracle) supplied by the caller.
//
// This translation unit uses ONLY the gaby_vm public API — no imported VIXL
// header — so it compiles as a clean, standalone unit. The VIXL bridging (the
// reference Simulator, RegisterDump core, MacroAssembler) lives in
// gaby_two_track_macros.h.
#ifndef GABY_VM_TEST_VIXL_ASM_VIXL_PORT_ORACLE_H_
#define GABY_VM_TEST_VIXL_ASM_VIXL_PORT_ORACLE_H_

#include <cstddef>
#include <cstdint>

#include "gaby_vm/registers.h"
#include "gaby_vm/simulator.h"

namespace gaby_vm::vixl_port_live {

// Seat entry state on a gaby Simulator: full RegisterFile from `entry`, then sp
// -> the top of `stack`, LR -> the null sentinel. Matches the frozen runner's
// SeatEntry: the harness owns sp/LR; everything else is the VIXL reference
// sim's post-ResetState snapshot.
void SeatEntry(Simulator& sim,
               const RegisterFile& entry,
               void* stack,
               std::size_t stack_size);

// 16-byte-down-aligned top of a stack buffer, used as the replay SP.
std::uint64_t StackTop(void* stack, std::size_t stack_size);

// Absolute-oracle per-assertion checks. Each compares one field of a post-run
// snapshot `got` against `want` (the VIXL reference sim's body-exit value).
// Returns true on match; on mismatch prints a [FAIL] line locating the case,
// track, and register, and returns false. `code` is the VIXL register code.
bool CheckX(const RegisterFile& got,
            unsigned code,
            std::uint64_t want,
            const char* track,
            const char* name);
bool CheckW(const RegisterFile& got,
            unsigned code,
            std::uint64_t want,
            const char* track,
            const char* name);
bool CheckNzcv(const RegisterFile& got,
               std::uint32_t want,
               const char* track,
               const char* name);
bool CheckFP16(const RegisterFile& got,
               unsigned code,
               std::uint16_t want_bits,
               const char* track,
               const char* name);
bool CheckFP32(const RegisterFile& got,
               unsigned code,
               std::uint32_t want_bits,
               const char* track,
               const char* name);
bool CheckFP64(const RegisterFile& got,
               unsigned code,
               std::uint64_t want_bits,
               const char* track,
               const char* name);
bool CheckV128(const RegisterFile& got,
               unsigned code,
               std::uint64_t want_lo,
               std::uint64_t want_hi,
               const char* track,
               const char* name);

// Differential oracle: full RegisterFile equality between the two tracks. pc
// and btype are excluded for the same reasons as the frozen runner (pc is the
// terminal sentinel; btype is a transient BTI tracker upstream does not
// assert).
bool DifferentialEqual(const RegisterFile& cache,
                       const RegisterFile& decoder,
                       const char* name);

// One-time startup equivalence assertion (tasks.md 4.2): a gaby Simulator
// seeded from the VIXL reference sim's ResetState snapshot must read back
// field- equivalent to that snapshot, in every field SeatEntry does not
// deliberately override (sp and x30/LR are harness-owned, so they are
// excluded). Prints a [FAIL] line and returns false on any divergence.
bool EntrySeedingEquivalent(const RegisterFile& reference_reset,
                            const RegisterFile& gaby_readback,
                            const char* context);

}  // namespace gaby_vm::vixl_port_live

#endif  // GABY_VM_TEST_VIXL_ASM_VIXL_PORT_ORACLE_H_
