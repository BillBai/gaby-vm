// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef GABY_VM_SIMULATOR_H_
#define GABY_VM_SIMULATOR_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

#include "gaby_vm/registers.h"

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

class Simulator {
 public:
  // Minimum size of the embedder-supplied stack buffer, in bytes. The
  // constructor aborts when stack_size is below this floor. The value tracks
  // the imported vixl::aarch64::SimStack defaults
  // (limit_guard_size_ + usable_size_ in src/aarch64/simulator-aarch64.h),
  // which is what VIXL itself treats as a reasonably-sized guest stack. A
  // static_assert in simulator.cc keeps the two in lockstep.
  static constexpr size_t kMinStackSize = 12 * 1024;

  // Construct a Simulator.
  //
  //   cache        — the PredecodeCache backing the cache track. May be null;
  //                  a null-cache Simulator supports only the debug track, and
  //                  a cache-track call on it aborts with a diagnostic.
  //   stack_buffer — embedder-owned memory the guest uses as its stack. The
  //                  initial SP is set to the (16-byte-aligned) top of this
  //                  buffer. Must outlive the Simulator.
  //   stack_size   — size of stack_buffer in bytes. MUST be at least
  //                  kMinStackSize; a smaller value aborts the constructor
  //                  with a diagnostic that names the rejected size and the
  //                  floor.
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
  // The no-argument form continues from the current PC — pair it with a
  // Write(GpRegister::PC, …) to drive a top-level single-stepping loop. The
  // entry_pc form seats the PC and steps in one call: it is the form a
  // re-entrant caller (one stepping from inside a memory-write observer) must
  // use, because it seats the PC inside the re-entrancy guard. Seating it with
  // a bare PC write first would corrupt the enclosing run — see the PC-case
  // note on Write(GpRegister, uint64_t).
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

  // --- Typed register I/O (shared by both tracks) ----------------------------
  //
  // Typed enum-keyed Read/Write surface. A V-register code cannot reach an
  // X-register accessor (or vice versa) because the parameter types differ.
  // Each Write updates the same backing slot the corresponding instruction
  // semantics would reach; each Read returns the current value of that slot.
  // Constructing an enum value via `static_cast` from an out-of-range integer
  // and passing it here aborts with a diagnostic.

  // General-purpose / SP / PC register access. Routing inside the simulator:
  //   X0..X30 — the corresponding X-register slot (`LR` aliases `X30`).
  //   SP      — the dedicated stack-pointer slot (distinct from the XZR
  //             encoding of code 31).
  //   PC      — the program counter, as a host address.
  //
  // PC case re-entrancy hazard. A Write(GpRegister::PC, …) only seats the PC;
  // it does not execute. Use it to seed a top-level run before a StepOnce() /
  // DebugStepOnce() loop. It is NOT a safe way to seat a *nested* step from
  // inside a leaf: it mutates the PC before any execution call can snapshot
  // the enclosing run's cursor, so the enclosing run would resume at the wrong
  // PC. A re-entrant caller seats its entry point through RunFrom,
  // StepOnce(entry_pc), or DebugStepOnce(entry_pc) instead, all of which seat
  // the PC inside the re-entrancy guard.
  void Write(GpRegister reg, uint64_t value);
  uint64_t Read(GpRegister reg) const;

  // FP/SIMD register V0..V31, full 128 bits.
  void Write(VRegister reg, VRegisterValue value);
  VRegisterValue Read(VRegister reg) const;

  // System register access. NZCV is packed in the architectural N,Z,C,V
  // layout. FPCR / FPSR are the floating-point control / status registers.
  // BType carries the BTI tracking state; a Write here promotes the value so
  // it is observable on the next Read (matching the typed-API round-trip
  // contract). The imported simulator does not model FPSR; the typed surface
  // keeps a slot for it so Write/Read round-trips behave like every other
  // sysreg.
  void Write(SysRegister reg, uint32_t value);
  uint32_t Read(SysRegister reg) const;

  // --- Bulk register I/O -----------------------------------------------------

  // Snapshot the full guest architectural state into a RegisterFile. Every
  // field of the returned RegisterFile is populated from the current value of
  // the corresponding register slot.
  RegisterFile ReadAll() const;

  // Restore the full guest architectural state from a RegisterFile. Every
  // slot is updated from the corresponding field; the observable effect
  // equals a sequence of individual typed Writes covering each slot exactly
  // once.
  //
  // WriteAll is a top-level state-restore entry point. It MUST NOT be called
  // from inside a leaf executed by an enclosing run: like a bare
  // Write(GpRegister::PC, …), it mutates the PC outside the re-entrancy
  // guard, which would corrupt the enclosing run's cursor.
  void WriteAll(const RegisterFile& file);

  // Apply a sequence of partial writes in span order. Each element's variant
  // alternative selects the matching typed Write; the call is observably
  // equivalent to invoking those single-element Writes in order. There is no
  // atomic-commit guarantee — an element that aborts (e.g. one carrying an
  // out-of-range enum value) leaves entries before it applied.
  //
  // This entry point is a C++ ergonomic convenience: `std::variant` and
  // `std::span` are not ABI-stable across stdlib versions, so FFI consumers
  // should drive partial writes through the individual typed Write overloads
  // or restore full state through WriteAll instead.
  void Write(std::span<const RegisterWrite> writes);

  // --- Debug-track configuration ---------------------------------------------
  // These affect only the debug track. On the cache track they are silently
  // ignored — choosing RunFrom is the embedder's promise that trace output is
  // not wanted (see design doc 4.1).

  // Set the trace parameter bitmask (vixl::aarch64::TraceParameters values:
  // LOG_DISASM, LOG_REGS, ...). Passing 0 disables tracing.
  void SetTraceParameters(int parameters);

  // --- Memory-write observation ----------------------------------------------

  // Payload delivered to a MemoryWriteObserver — a single guest memory write
  // event, by value. Lifetime is the observer callback itself.
  struct MemoryWriteEvent {
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
  // nested entry point safely (see the PC-case note on
  // Write(GpRegister, uint64_t)).
  using MemoryWriteObserver = std::function<void(const MemoryWriteEvent&)>;
  void SetMemoryWriteObserver(MemoryWriteObserver observer);

  // --- Branch observation ----------------------------------------------------

  // Hook fired on every taken PC-redirecting branch the guest executes, on
  // both the cache track and the debug track, after target resolution and
  // before PC commit. The branch families covered are: B, BL, B.cond (when
  // the condition passes), TBZ/TBNZ (when the branch is taken), CBZ/CBNZ
  // (when the branch is taken), BR, BLR, RET, and the PAC variants BRAA,
  // BRAB, BLRAA, BLRAB, RETAA, RETAB. Not-taken conditional, test, and
  // compare branches fall through to PC+4 and do NOT invoke the hook.
  //
  // The hook returns the address the simulator should commit as PC:
  //   return target_pc  — identity: continue at the architectural target.
  //                       With no other side effects in the hook body this
  //                       is observationally indistinguishable from running
  //                       with no hook installed.
  //   return other_pc   — divert: commit a different PC. On the cache
  //                       track, other_pc must lie inside some registered
  //                       code range; otherwise the next cache step aborts
  //                       with the existing "PC not in any registered code
  //                       range" diagnostic.
  //   return 0          — terminate the run. The simulator commits PC = 0
  //                       (the null-LR end-of-simulation sentinel), which
  //                       trips IsSimulationFinished on the next StepOnce /
  //                       DebugStepOnce and the enclosing run returns. This
  //                       is the same termination path the guest uses with
  //                       a RET to a null LR; no separate "stop" flag.
  //
  // Arguments:
  //   target_pc — the architectural target address. For PAC variants this is
  //               the authenticated (stripped) address, not the raw signed
  //               value. For BL-class branches the simulator has already
  //               written LR to point at the post-branch return address by
  //               the time the hook runs, so sim.Read(GpRegister::LR) inside
  //               the hook observes it.
  //   user_data — the opaque pointer the embedder passed to SetBranchHook.
  //   sim       — the same Simulator this hook is installed on, exposed so
  //               the hook body can read/write registers and seat nested
  //               execution calls (see SetBranchHook for the re-entrancy
  //               contract).
  using BranchHook = uintptr_t (*)(uintptr_t target_pc,
                                   void* user_data,
                                   Simulator& sim);

  // Install (or atomically replace) the branch hook. Passing nullptr for
  // `hook` removes it; user_data is then ignored. The same hook applies to
  // both execution tracks — the embedder does not need to know which track
  // is in use.
  //
  // Threading. SetBranchHook is not thread-safe relative to a concurrent
  // execution call on the same Simulator. The embedder MUST install or
  // replace the hook either before any execution call begins on this
  // Simulator, or after the current execution call has returned. This
  // matches the existing single-writer threading contract for Simulator.
  //
  // Re-entrancy contract. From inside the hook body, the embedder MAY
  // perform any of the following on the same Simulator:
  //   - any typed Read(...) overload;
  //   - any typed Write(...) overload EXCEPT Write(GpRegister::PC, _);
  //   - seat a nested run or step via RunFrom(entry_pc),
  //     StepOnce(entry_pc), or DebugStepOnce(entry_pc).
  // The embedder MUST NOT, from inside the hook body, do either of:
  //   - call Write(GpRegister::PC, _) followed by bare StepOnce() or
  //     DebugStepOnce() — the bare PC write mutates the simulator's PC
  //     outside the re-entrancy guard and corrupts the enclosing run's
  //     cursor on resume (the same hazard the PC-case note on
  //     Write(GpRegister, uint64_t) calls out);
  //   - call WriteAll(_) — same reason: WriteAll is a top-level state
  //     restore that mutates PC outside the guard.
  // These are the same rules the existing MemoryWriteObserver follows.
  void SetBranchHook(BranchHook hook, void* user_data);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gaby_vm

#endif  // GABY_VM_SIMULATOR_H_
