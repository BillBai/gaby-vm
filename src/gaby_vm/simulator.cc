// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#include "gaby_vm/simulator.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include "cpu-features.h"
#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/registers.h"
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

// Overload-set helper for std::visit. Lets the span-write loop dispatch on the
// variant alternative with one expression per alternative — the standard
// "overloaded lambdas" idiom.
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

[[noreturn]] void AbortSpanElement(size_t index,
                                   const char* enum_name,
                                   unsigned bad_value) {
  char msg[160];
  std::snprintf(msg,
                sizeof(msg),
                "gaby_vm::Simulator::Write(std::span<RegisterWrite>): "
                "element %zu carries %s with out-of-range underlying value %u",
                index,
                enum_name,
                bad_value);
  VIXL_ABORT_WITH_MSG(msg);
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
  // FPSR is not modeled by the imported simulator (it tracks no cumulative FP
  // exception state). The typed Read/Write surface promises a round-trip on
  // every sysreg slot, so we keep our own FPSR storage here.
  uint32_t fpsr_storage = 0;
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
  // enclosing run's saved cursor (simulator.h, Write(GpRegister::PC, …)).
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
  // StepOnce(entry_pc) rather than Write(GpRegister::PC, …) + StepOnce()
  // (simulator.h).
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


// --- Typed register I/O ------------------------------------------------------

void Simulator::Write(GpRegister reg, uint64_t value) {
  const uint8_t code = static_cast<uint8_t>(reg);
  if (code <= 30) {
    impl_->vsim.WriteXRegister(code, static_cast<int64_t>(value));
  } else if (reg == GpRegister::SP) {
    impl_->vsim.WriteSp(value);
  } else if (reg == GpRegister::PC) {
    impl_->vsim.WritePc(AsInstruction(static_cast<uintptr_t>(value)),
                        vixl::aarch64::Simulator::NoBranchLog);
  } else {
    char msg[128];
    std::snprintf(msg,
                  sizeof(msg),
                  "gaby_vm::Simulator::Write(GpRegister, …): out-of-range "
                  "underlying value %u",
                  static_cast<unsigned>(code));
    VIXL_ABORT_WITH_MSG(msg);
  }
}

uint64_t Simulator::Read(GpRegister reg) const {
  const uint8_t code = static_cast<uint8_t>(reg);
  if (code <= 30) {
    return static_cast<uint64_t>(impl_->vsim.ReadXRegister(code));
  } else if (reg == GpRegister::SP) {
    return static_cast<uint64_t>(
        impl_->vsim.ReadXRegister(31, vixl::aarch64::Reg31IsStackPointer));
  } else if (reg == GpRegister::PC) {
    return static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(impl_->vsim.ReadPc()));
  }
  char msg[128];
  std::snprintf(msg,
                sizeof(msg),
                "gaby_vm::Simulator::Read(GpRegister) const: out-of-range "
                "underlying value %u",
                static_cast<unsigned>(code));
  VIXL_ABORT_WITH_MSG(msg);
}

void Simulator::Write(VRegister reg, VRegisterValue value) {
  const uint8_t code = static_cast<uint8_t>(reg);
  if (code > 31) {
    char msg[128];
    std::snprintf(msg,
                  sizeof(msg),
                  "gaby_vm::Simulator::Write(VRegister, …): out-of-range "
                  "underlying value %u",
                  static_cast<unsigned>(code));
    VIXL_ABORT_WITH_MSG(msg);
  }
  vixl::aarch64::Simulator::qreg_t q{};
  std::memcpy(&q.val[0], &value.lo, sizeof(value.lo));
  std::memcpy(&q.val[sizeof(value.lo)], &value.hi, sizeof(value.hi));
  impl_->vsim.WriteQRegister(code, q);
}

VRegisterValue Simulator::Read(VRegister reg) const {
  const uint8_t code = static_cast<uint8_t>(reg);
  if (code > 31) {
    char msg[128];
    std::snprintf(msg,
                  sizeof(msg),
                  "gaby_vm::Simulator::Read(VRegister) const: out-of-range "
                  "underlying value %u",
                  static_cast<unsigned>(code));
    VIXL_ABORT_WITH_MSG(msg);
  }
  vixl::aarch64::Simulator::qreg_t q = impl_->vsim.ReadQRegister(code);
  VRegisterValue value{};
  std::memcpy(&value.lo, &q.val[0], sizeof(value.lo));
  std::memcpy(&value.hi, &q.val[sizeof(value.lo)], sizeof(value.hi));
  return value;
}

void Simulator::Write(SysRegister reg, uint32_t value) {
  switch (reg) {
    case SysRegister::NZCV:
      // VIXL initializes nzcv_ via DefaultValueFor(NZCV) in
      // ResetSystemRegisters, so its write_ignore_mask exposes exactly the
      // architectural N/Z/C/V bits (31..28) for SetRawValue.
      impl_->vsim.ReadNzcv().SetRawValue(value);
      break;
    case SysRegister::FPCR:
      // Same shape as NZCV: DefaultValueFor(FPCR) installs the architectural
      // write-ignore mask, so SetRawValue updates only the writable bits.
      impl_->vsim.ReadFpcr().SetRawValue(value);
      break;
    case SysRegister::FPSR:
      // VIXL does not model FPSR; the typed surface promises a round-trip, so
      // we keep our own slot in Impl.
      impl_->fpsr_storage = value;
      break;
    case SysRegister::BType: {
      // WriteNextBType + UpdateBType promotes the supplied value into the
      // simulator's btype_ slot, so a subsequent Read sees it immediately.
      // Outside instruction execution there is no pending staged BType to
      // clobber, so resetting next_btype_ to DefaultBType (UpdateBType's side
      // effect) is the correct steady state.
      const uint8_t b = static_cast<uint8_t>(value);
      if (b > 3) {
        char msg[128];
        std::snprintf(msg,
                      sizeof(msg),
                      "gaby_vm::Simulator::Write(SysRegister::BType, …): "
                      "BType value %u is outside the [0,3] range",
                      static_cast<unsigned>(b));
        VIXL_ABORT_WITH_MSG(msg);
      }
      impl_->vsim.WriteNextBType(static_cast<vixl::aarch64::BType>(b));
      impl_->vsim.UpdateBType();
      break;
    }
    default: {
      char msg[128];
      std::snprintf(msg,
                    sizeof(msg),
                    "gaby_vm::Simulator::Write(SysRegister, …): out-of-range "
                    "underlying value %u",
                    static_cast<unsigned>(static_cast<uint8_t>(reg)));
      VIXL_ABORT_WITH_MSG(msg);
    }
  }
}

uint32_t Simulator::Read(SysRegister reg) const {
  switch (reg) {
    case SysRegister::NZCV: {
      // Decompose from the bit accessors: const-friendly and independent of
      // any vagaries in the SimSystemRegister's writable-bit mask.
      vixl::aarch64::Simulator& s = impl_->vsim;
      return ((s.ReadN() ? 1u : 0u) << 31) | ((s.ReadZ() ? 1u : 0u) << 30) |
             ((s.ReadC() ? 1u : 0u) << 29) | ((s.ReadV() ? 1u : 0u) << 28);
    }
    case SysRegister::FPCR:
      return impl_->vsim.ReadFpcr().GetRawValue();
    case SysRegister::FPSR:
      return impl_->fpsr_storage;
    case SysRegister::BType:
      return static_cast<uint32_t>(impl_->vsim.ReadBType());
  }
  char msg[128];
  std::snprintf(msg,
                sizeof(msg),
                "gaby_vm::Simulator::Read(SysRegister) const: out-of-range "
                "underlying value %u",
                static_cast<unsigned>(static_cast<uint8_t>(reg)));
  VIXL_ABORT_WITH_MSG(msg);
}


// --- Bulk register I/O -------------------------------------------------------

RegisterFile Simulator::ReadAll() const {
  RegisterFile file{};
  for (unsigned i = 0; i <= 30; ++i) {
    file.x[i] = Read(static_cast<GpRegister>(i));
  }
  file.sp = Read(GpRegister::SP);
  file.pc = Read(GpRegister::PC);
  for (unsigned i = 0; i <= 31; ++i) {
    file.v[i] = Read(static_cast<VRegister>(i));
  }
  file.nzcv = Read(SysRegister::NZCV);
  file.fpcr = Read(SysRegister::FPCR);
  file.fpsr = Read(SysRegister::FPSR);
  file.btype = Read(SysRegister::BType);
  return file;
}

void Simulator::WriteAll(const RegisterFile& file) {
  for (unsigned i = 0; i <= 30; ++i) {
    Write(static_cast<GpRegister>(i), file.x[i]);
  }
  Write(GpRegister::SP, file.sp);
  Write(GpRegister::PC, file.pc);
  for (unsigned i = 0; i <= 31; ++i) {
    Write(static_cast<VRegister>(i), file.v[i]);
  }
  Write(SysRegister::NZCV, file.nzcv);
  Write(SysRegister::FPCR, file.fpcr);
  Write(SysRegister::FPSR, file.fpsr);
  Write(SysRegister::BType, file.btype);
}

void Simulator::Write(std::span<const RegisterWrite> writes) {
  for (size_t i = 0; i < writes.size(); ++i) {
    std::visit(overloaded{
                   [this, i](const GpWrite& w) {
                     const uint8_t code = static_cast<uint8_t>(w.reg);
                     if (code > 32) {
                       AbortSpanElement(i, "GpRegister", code);
                     }
                     this->Write(w.reg, w.value);
                   },
                   [this, i](const VWrite& w) {
                     const uint8_t code = static_cast<uint8_t>(w.reg);
                     if (code > 31) {
                       AbortSpanElement(i, "VRegister", code);
                     }
                     this->Write(w.reg, w.value);
                   },
                   [this, i](const SysWrite& w) {
                     const uint8_t code = static_cast<uint8_t>(w.reg);
                     if (code > 3) {
                       AbortSpanElement(i, "SysRegister", code);
                     }
                     this->Write(w.reg, w.value);
                   },
               },
               writes[i]);
  }
}


// --- Debug-track configuration / memory observer -----------------------------

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
