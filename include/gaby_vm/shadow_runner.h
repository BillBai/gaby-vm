// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef GABY_VM_SHADOW_RUNNER_H_
#define GABY_VM_SHADOW_RUNNER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

// gaby_vm::testing::ShadowRunner — the V1 correctness oracle for the cache.
//
// ShadowRunner executes the same guest code twice in lockstep: once on the
// cache track and once on the imported decoder track, over a shared stack
// buffer so register values are directly comparable. After every instruction
// it compares the full architectural state of the two tracks. The first
// disagreement is reported as a DivergenceReport.
//
// It lives in namespace gaby_vm::testing because it is a diagnosis / test tool,
// not a production execution path: lockstep dual execution is far slower than
// either track alone. This header exposes no vixl:: type.

namespace gaby_vm {

class PredecodeCache;

namespace testing {

// A single point at which the cache track and the debug track disagreed.
//
// Plain-old-data, and fully self-contained: every field — including the
// `reg_name` text, which is stored inline rather than as a pointer — is held
// by value, so a handler may copy a report and keep it after the ShadowRunner
// that produced it is gone. Which fields are meaningful depends on `kind`.
struct DivergenceReport {
  enum class Kind {
    // A general / FP register, the PC, or a flag/control register differs.
    Register,
    // The two tracks performed a different memory write for this instruction.
    MemoryWrite,
  };

  // What kind of state diverged.
  Kind kind;
  // Host address of the instruction whose execution produced the divergence.
  uintptr_t pc;

  // --- Register divergence (kind == Register) --------------------------------
  // Index of the differing register, or -1 for a non-indexed item (PC, NZCV,
  // FPCR, FPSR, BType).
  int reg_index;
  // Human-readable name of the differing item, e.g. "X3", "V7", "NZCV", "PC",
  // as a NUL-terminated string stored inline. Empty ("") when kind != Register.
  char reg_name[8];
  // Differing value on the cache track and the debug track. For a 128-bit V
  // register, `*_hi` carries bits [127:64]; otherwise `*_hi` is 0.
  uint64_t fast_lo;
  uint64_t fast_hi;
  uint64_t ref_lo;
  uint64_t ref_hi;

  // --- Memory-write divergence (kind == MemoryWrite) -------------------------
  // Address and size in bytes of the differing write. `mem_addr` is 0 and
  // `mem_size` is 0 when kind != MemoryWrite.
  uintptr_t mem_addr;
  size_t mem_size;
  // Value written by each track (lo/hi halves for writes wider than 64 bits).
  uint64_t mem_fast_lo;
  uint64_t mem_fast_hi;
  uint64_t mem_ref_lo;
  uint64_t mem_ref_hi;
};

// Invoked on the first divergence. The default handler dumps the report to
// stderr and aborts; installing a custom handler (SetDivergenceHandler) makes
// divergence non-fatal — used by the oracle's own self-test.
using DivergenceHandler = std::function<void(const DivergenceReport&)>;

class ShadowRunner {
 public:
  // Construct a ShadowRunner over `cache`. `stack_buffer` / `stack_size` is the
  // single stack buffer shared by both internal Simulators, so their SP — and
  // every stack-relative pointer — compares byte-equal. Must outlive the
  // ShadowRunner.
  ShadowRunner(PredecodeCache* cache, void* stack_buffer, size_t stack_size);
  ~ShadowRunner();

  // Not copyable: it owns two Simulators and a shared comparison harness.
  ShadowRunner(const ShadowRunner&) = delete;
  ShadowRunner& operator=(const ShadowRunner&) = delete;

  // Mirror a general-purpose register write to both internal Simulators, so
  // they start from identical state. `code` in [0, 30].
  void WriteXRegister(unsigned code, uint64_t value);
  uint64_t ReadXRegister(unsigned code) const;

  // Mirror a stack-pointer write to both internal Simulators.
  void WriteSp(uint64_t value);
  uint64_t ReadSp() const;

  // Execute from `entry_pc` on both tracks in lockstep, comparing
  // architectural state after every instruction. On the first divergence the
  // installed handler is invoked.
  void RunFrom(uintptr_t entry_pc);

  // Install a custom divergence handler, replacing the default abort-on-diff
  // behaviour. Passing an empty std::function restores the default.
  void SetDivergenceHandler(DivergenceHandler handler);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace testing
}  // namespace gaby_vm

#endif  // GABY_VM_SHADOW_RUNNER_H_
