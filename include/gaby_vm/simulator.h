// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef GABY_VM_SIMULATOR_H_
#define GABY_VM_SIMULATOR_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// gaby_vm::Simulator — the dual-track AArch64 user-mode interpreter.
//
// One Simulator owns one guest architectural state (general registers, FP/SIMD
// registers, PC, flags, ...) and executes it through one of two tracks:
//
//   * the cache track  (RunFrom / StepOnce)       — dispatches through a
//     PredecodeCache; fast; no trace / debug observability.
//   * the debug track  (DebugRunFrom / DebugStepOnce) — dispatches through the
//     imported VIXL decoder + visitor chain; slower; fully observable.
//
// A single execution call stays on one track for its whole duration; the
// simulator never switches tracks mid-run. For identical code and identical
// initial state the two tracks produce identical architectural results — that
// equivalence is what the ShadowRunner oracle (shadow_runner.h) checks.
//
// This header is part of the gaby-vm public API: it exposes no vixl:: type and
// includes no imported VIXL header. The implementation is Pimpl-wrapped so the
// imported vixl::aarch64::Simulator stays hidden from embedders.

namespace gaby_vm {

class PredecodeCache;

// The full 128-bit value of one FP/SIMD (V) register, split into two 64-bit
// halves. Returned by Simulator::ReadVRegister so callers — notably
// ShadowRunner — can compare V registers over their entire width.
struct VRegisterValue {
  uint64_t lo;  // bits [63:0]
  uint64_t hi;  // bits [127:64]
};

class Simulator {
 public:
  // Construct a Simulator.
  //
  //   cache        — the PredecodeCache backing the cache track. May be null;
  //                  a null-cache Simulator supports only the debug track, and
  //                  a cache-track call on it aborts with a diagnostic.
  //   stack_buffer — embedder-owned memory the guest uses as its stack. The
  //                  initial SP is set to the (16-byte-aligned) top of this
  //                  buffer. Must outlive the Simulator.
  //   stack_size   — size of stack_buffer in bytes.
  Simulator(PredecodeCache* cache, void* stack_buffer, size_t stack_size);
  ~Simulator();

  // Not copyable: a Simulator owns mutable guest state and imported resources.
  Simulator(const Simulator&) = delete;
  Simulator& operator=(const Simulator&) = delete;

  // --- Cache track -----------------------------------------------------------
  // Requires a non-null cache; otherwise aborts with a diagnostic. A PC that
  // leaves every registered code range aborts (no silent fallback).

  // Execute from `entry_pc` until the simulation terminates (PC reaches the
  // end-of-simulation sentinel — e.g. a RET to a null link register).
  void RunFrom(uintptr_t entry_pc);

  // Execute exactly one instruction on the cache track. Returns false once the
  // simulation has terminated (nothing was executed), true otherwise.
  //
  // The no-argument form continues from the current PC — pair it with WritePc
  // to drive a top-level single-stepping loop. The entry_pc form seats the PC
  // and steps in one call: it is the form a re-entrant caller (one stepping
  // from inside a memory-write observer) must use, because it seats the PC
  // inside the re-entrancy guard. Seating it with a bare WritePc first would
  // corrupt the enclosing run — see WritePc.
  bool StepOnce();
  bool StepOnce(uintptr_t entry_pc);

  // --- Debug track -----------------------------------------------------------
  // Always available, including on a null-cache Simulator. Dispatches through
  // the imported Decoder -> VisitNamedInstruction -> leaf flow.

  // Execute from `entry_pc` until the simulation terminates.
  void DebugRunFrom(uintptr_t entry_pc);

  // Execute exactly one instruction on the debug track. Returns false once the
  // simulation has terminated, true otherwise. The two forms mirror StepOnce:
  // the no-argument form continues from the current PC, the entry_pc form
  // seats the PC inside the re-entrancy guard for a re-entrant caller.
  bool DebugStepOnce();
  bool DebugStepOnce(uintptr_t entry_pc);

  // --- Guest register / PC access (shared by both tracks) --------------------

  // General-purpose registers X0..X30 (`code` in [0, 30]).
  void WriteXRegister(unsigned code, uint64_t value);
  uint64_t ReadXRegister(unsigned code) const;

  // Stack pointer. VIXL models SP separately from the XZR encoding of code 31,
  // so it gets its own accessors.
  void WriteSp(uint64_t value);
  uint64_t ReadSp() const;

  // Program counter as a host address.
  //
  // WritePc only seats the PC; it does not execute. Use it to seed a top-level
  // run before a StepOnce() / DebugStepOnce() loop. It is NOT a safe way to
  // seat a *nested* step from inside a leaf: it mutates the PC before any
  // execution call can snapshot the enclosing run's cursor, so the enclosing
  // run would resume at the wrong PC. A re-entrant caller seats its entry point
  // through RunFrom / StepOnce(entry_pc) / DebugStepOnce(entry_pc) instead,
  // which all seat the PC inside the re-entrancy guard.
  void WritePc(uintptr_t pc);
  uintptr_t ReadPc() const;

  // FP/SIMD register V0..V31 (`code` in [0, 31]), full 128 bits.
  VRegisterValue ReadVRegister(unsigned code) const;

  // Condition flags (N, Z, C, V) packed in the architectural NZCV layout.
  uint32_t ReadNzcv() const;

  // Floating-point control / status registers.
  uint32_t ReadFpcr() const;
  uint32_t ReadFpsr() const;

  // Branch-type register, used for BTI tracking.
  uint32_t ReadBType() const;

  // --- Debug-track configuration ---------------------------------------------
  // These affect only the debug track. On the cache track they are silently
  // ignored — choosing RunFrom is the embedder's promise that trace output is
  // not wanted (see design doc 4.1).

  // Set the trace parameter bitmask (vixl::aarch64::TraceParameters values:
  // LOG_DISASM, LOG_REGS, ...). Passing 0 disables tracing.
  void SetTraceParameters(int parameters);

  // --- Memory-write observation ----------------------------------------------

  // A single guest memory write.
  struct MemoryWrite {
    uintptr_t address;
    size_t size;        // bytes: 1, 2, 4, 8, or 16
    uint64_t value_lo;  // low 64 bits of the stored value
    uint64_t value_hi;  // bits [127:64] for a 128-bit SIMD write, else 0
  };

  // Observer invoked for every guest memory write — on either track — as the
  // store executes, from within the store instruction's leaf. ShadowRunner
  // installs one to compare per-instruction memory writes between the tracks.
  // Passing an empty std::function removes the observer (the default state, in
  // which stores incur no observation overhead). The observer MAY make
  // re-entrant execution calls on this Simulator — via RunFrom,
  // StepOnce(entry_pc) or DebugStepOnce(entry_pc), the forms that seat the
  // nested entry point safely (see WritePc).
  using MemoryWriteObserver = std::function<void(const MemoryWrite&)>;
  void SetMemoryWriteObserver(MemoryWriteObserver observer);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gaby_vm

#endif  // GABY_VM_SIMULATOR_H_
