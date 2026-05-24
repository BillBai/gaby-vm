// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#include "gaby_vm/simulator.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "cpu-features.h"
#include "gaby_vm/predecode_cache.h"
#include "globals-vixl.h"

#include "aarch64/decoder-aarch64.h"
#include "aarch64/instructions-aarch64.h"
#include "aarch64/simulator-aarch64.h"

namespace gaby_vm {
namespace {

// RAII guard for one Simulator execution call. The single imported
// vixl::aarch64::Simulator is shared by both tracks and by nested calls; this
// guard pins the two pieces of per-call state that sharing would otherwise
// corrupt.
//
// Re-entrancy (predecode-cache-core design.md D10). `busy` records whether an
// execution call on this Simulator is already in progress. The OUTERMOST call
// flips it true on entry / false on exit and does NOT touch the interpreter
// cursor: its instruction-stream progress must persist (a top-level StepOnce
// has to actually advance). A NESTED call — one entered from inside a leaf of
// an enclosing call — instead snapshots the enclosing run's cursor on entry
// and restores it on return, so the enclosing run resumes exactly where it
// left off. The guest register file is deliberately not part of the cursor: a
// nested call's register effects flow back to the enclosing run, exactly as
// across a real call boundary.
//
// Trace. Trace output is a debug-track-only feature (simulator.h): the cache
// track promises none. Because both tracks share one imported Simulator, each
// call sets that Simulator's trace bitmask for its own duration — the
// embedder's configured mask for a debug-track call, 0 for a cache-track call
// — and restores the previous value on exit. Restoring (rather than zeroing)
// keeps a nested call from disturbing an enclosing debug-track run's tracing.
class ExecutionScope {
 public:
  ExecutionScope(vixl::aarch64::Simulator& sim,
                 bool& busy,
                 int trace_parameters)
      : sim_(sim),
        busy_(busy),
        nested_(busy),
        saved_trace_(sim.GetTraceParameters()) {
    if (nested_) {
      saved_cursor_ = sim_.GabySaveCursor();
    }
    busy_ = true;
    sim_.SetTraceParameters(trace_parameters);
  }

  ~ExecutionScope() {
    sim_.SetTraceParameters(saved_trace_);
    if (nested_) {
      sim_.GabyRestoreCursor(saved_cursor_);
    } else {
      busy_ = false;
    }
  }

  ExecutionScope(const ExecutionScope&) = delete;
  ExecutionScope& operator=(const ExecutionScope&) = delete;

 private:
  vixl::aarch64::Simulator& sim_;
  bool& busy_;
  const bool nested_;
  const int saved_trace_;
  vixl::aarch64::Simulator::GabyInterpreterCursor saved_cursor_{};
};

// Adapts the imported vixl::aarch64::MemoryWriteSink interface to the gaby-vm
// public MemoryWriteObserver (a vixl-free std::function). One lives in each
// Impl; it is wired into the imported Simulator only while an observer is set.
class ForwardingWriteSink : public vixl::aarch64::MemoryWriteSink {
 public:
  void Record(uintptr_t addr,
              size_t size,
              uint64_t value_lo,
              uint64_t value_hi) override {
    if (observer) {
      observer(Simulator::MemoryWrite{addr, size, value_lo, value_hi});
    }
  }

  Simulator::MemoryWriteObserver observer;
};

const vixl::aarch64::Instruction* AsInstruction(uintptr_t pc) {
  return reinterpret_cast<const vixl::aarch64::Instruction*>(pc);
}

}  // namespace

// Holds the one imported vixl::aarch64::Simulator that backs both tracks: the
// cache track drives it through ExecuteInstructionCached, the debug track
// through the imported decoder. `decoder` is that Simulator's decoder, used by
// the debug track. `cache` is the (nullable) predecode cache.
class Simulator::Impl {
 public:
  Impl(PredecodeCache* cache_arg, void* stack_buffer, size_t stack_size)
      : vsim(&decoder), cache(cache_arg) {
    // Wire the predecode cache into the imported Simulator (null is fine — a
    // null-cache Simulator simply has no cache track).
    vsim.SetPredecodeCache(cache);
    // The debug track audits every instruction and aborts on a feature the
    // auditor rejects; All() keeps it from rejecting baseline instructions,
    // matching how a bare vixl::aarch64::Simulator is configured to execute.
    vsim.SetCPUFeatures(vixl::CPUFeatures::All());
    // Point the guest stack pointer at the top of the embedder's buffer,
    // 16-byte aligned down per AAPCS64. That buffer IS the guest stack; the
    // SimStack the imported Simulator allocates for itself goes unused (the
    // guest SP never points into it, so its guard regions never trip).
    uintptr_t top = reinterpret_cast<uintptr_t>(stack_buffer) + stack_size;
    top &= ~static_cast<uintptr_t>(15);
    vsim.WriteSp(top);
  }

  vixl::aarch64::Decoder decoder;
  // sink declared before vsim so it outlives vsim's write_sink_ pointer.
  ForwardingWriteSink sink;
  vixl::aarch64::Simulator vsim;
  PredecodeCache* cache;
  bool busy = false;
  // Embedder-configured trace bitmask. Held here, not pushed into `vsim`, so it
  // reaches only the debug track: each debug-track call applies it for its
  // duration, each cache-track call applies 0 (see ExecutionScope).
  int trace_parameters = 0;
};


Simulator::Simulator(PredecodeCache* cache,
                     void* stack_buffer,
                     size_t stack_size)
    : impl_(std::make_unique<Impl>(cache, stack_buffer, stack_size)) {}

Simulator::~Simulator() = default;


void Simulator::RunFrom(uintptr_t entry_pc) {
  if (impl_->cache == nullptr) {
    VIXL_ABORT_WITH_MSG(
        "gaby_vm::Simulator::RunFrom (cache track) called on a Simulator "
        "constructed without a PredecodeCache ");
  }
  // Cache track: trace is suppressed (the ExecutionScope trace mask is 0). The
  // PC is seated inside the scope so a nested RunFrom cannot corrupt an
  // enclosing run's saved cursor (simulator.h, WritePc).
  ExecutionScope scope(impl_->vsim, impl_->busy, 0);
  impl_->vsim.WritePc(AsInstruction(entry_pc),
                      vixl::aarch64::Simulator::NoBranchLog);
  while (impl_->vsim.StepOnce()) {
  }
}

bool Simulator::StepOnce() {
  if (impl_->cache == nullptr) {
    VIXL_ABORT_WITH_MSG(
        "gaby_vm::Simulator::StepOnce (cache track) called on a Simulator "
        "constructed without a PredecodeCache ");
  }
  ExecutionScope scope(impl_->vsim, impl_->busy, 0);
  return impl_->vsim.StepOnce();
}

bool Simulator::StepOnce(uintptr_t entry_pc) {
  if (impl_->cache == nullptr) {
    VIXL_ABORT_WITH_MSG(
        "gaby_vm::Simulator::StepOnce (cache track) called on a Simulator "
        "constructed without a PredecodeCache ");
  }
  // Seat the PC *inside* the scope: when this call is nested in an enclosing
  // run, the scope has already snapshotted that run's cursor, so overwriting
  // the PC here cannot corrupt it. This is why a re-entrant caller must use
  // StepOnce(entry_pc) rather than WritePc() + StepOnce() (simulator.h).
  ExecutionScope scope(impl_->vsim, impl_->busy, 0);
  impl_->vsim.WritePc(AsInstruction(entry_pc),
                      vixl::aarch64::Simulator::NoBranchLog);
  return impl_->vsim.StepOnce();
}

void Simulator::DebugRunFrom(uintptr_t entry_pc) {
  // Debug track: the embedder's configured trace mask applies.
  ExecutionScope scope(impl_->vsim, impl_->busy, impl_->trace_parameters);
  impl_->vsim.WritePc(AsInstruction(entry_pc),
                      vixl::aarch64::Simulator::NoBranchLog);
  while (impl_->vsim.DebugStepOnce()) {
  }
}

bool Simulator::DebugStepOnce() {
  ExecutionScope scope(impl_->vsim, impl_->busy, impl_->trace_parameters);
  return impl_->vsim.DebugStepOnce();
}

bool Simulator::DebugStepOnce(uintptr_t entry_pc) {
  // Seat the PC inside the scope, as StepOnce(entry_pc) does.
  ExecutionScope scope(impl_->vsim, impl_->busy, impl_->trace_parameters);
  impl_->vsim.WritePc(AsInstruction(entry_pc),
                      vixl::aarch64::Simulator::NoBranchLog);
  return impl_->vsim.DebugStepOnce();
}


void Simulator::WriteXRegister(unsigned code, uint64_t value) {
  impl_->vsim.WriteXRegister(code, static_cast<int64_t>(value));
}

uint64_t Simulator::ReadXRegister(unsigned code) const {
  return static_cast<uint64_t>(impl_->vsim.ReadXRegister(code));
}

void Simulator::WriteSp(uint64_t value) { impl_->vsim.WriteSp(value); }

uint64_t Simulator::ReadSp() const {
  return static_cast<uint64_t>(
      impl_->vsim.ReadXRegister(31, vixl::aarch64::Reg31IsStackPointer));
}

void Simulator::WritePc(uintptr_t pc) {
  impl_->vsim.WritePc(AsInstruction(pc), vixl::aarch64::Simulator::NoBranchLog);
}

uintptr_t Simulator::ReadPc() const {
  return reinterpret_cast<uintptr_t>(impl_->vsim.ReadPc());
}

VRegisterValue Simulator::ReadVRegister(unsigned code) const {
  vixl::aarch64::Simulator::qreg_t q = impl_->vsim.ReadQRegister(code);
  VRegisterValue value;
  std::memcpy(&value.lo, &q.val[0], sizeof(value.lo));
  std::memcpy(&value.hi, &q.val[sizeof(value.lo)], sizeof(value.hi));
  return value;
}

uint32_t Simulator::ReadNzcv() const {
  vixl::aarch64::Simulator& s = impl_->vsim;
  return ((s.ReadN() ? 1u : 0u) << 31) | ((s.ReadZ() ? 1u : 0u) << 30) |
         ((s.ReadC() ? 1u : 0u) << 29) | ((s.ReadV() ? 1u : 0u) << 28);
}

uint32_t Simulator::ReadFpcr() const {
  return impl_->vsim.ReadFpcr().GetRawValue();
}

uint32_t Simulator::ReadFpsr() const {
  // The imported VIXL simulator does not model FPSR (it tracks no cumulative
  // FP exception state), so there is nothing to read; it reports as 0. The
  // accessor exists so ShadowRunner's comparison set can name FPSR uniformly.
  return 0;
}

uint32_t Simulator::ReadBType() const {
  return static_cast<uint32_t>(impl_->vsim.ReadBType());
}

void Simulator::SetTraceParameters(int parameters) {
  // Store only; ExecutionScope applies it to the imported simulator around
  // debug-track calls and forces 0 around cache-track calls, so trace output
  // never leaks into the cache track (simulator.h).
  impl_->trace_parameters = parameters;
}

void Simulator::SetMemoryWriteObserver(MemoryWriteObserver observer) {
  impl_->sink.observer = std::move(observer);
  // Wire the sink into the imported Simulator only while an observer is set,
  // so stores pay nothing for observation when none is installed.
  impl_->vsim.SetMemoryWriteSink(impl_->sink.observer ? &impl_->sink : nullptr);
}

}  // namespace gaby_vm
