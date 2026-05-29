// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#include "gaby_vm/shadow_runner.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "gaby_vm/predecode_cache.h"
#include "gaby_vm/simulator.h"

namespace gaby_vm {
namespace testing {
namespace {

// Default divergence handler: dump the diff to stderr and abort. A custom
// handler installed via SetDivergenceHandler replaces this — that is what the
// oracle's own self-test uses to make divergence observable but non-fatal.
void DefaultDivergenceHandler(const DivergenceReport& report) {
  std::fprintf(stderr,
               "\nShadowRunner: cache / decoder divergence detected.\n");
  std::fprintf(stderr,
               "  instruction pc : 0x%016llx\n",
               static_cast<unsigned long long>(report.pc));
  if (report.kind == DivergenceReport::Kind::Register) {
    std::fprintf(stderr,
                 "  register       : %s\n"
                 "  cache track    : 0x%016llx%016llx\n"
                 "  decoder track  : 0x%016llx%016llx\n",
                 report.reg_name,
                 static_cast<unsigned long long>(report.fast_hi),
                 static_cast<unsigned long long>(report.fast_lo),
                 static_cast<unsigned long long>(report.ref_hi),
                 static_cast<unsigned long long>(report.ref_lo));
  } else {
    std::fprintf(stderr,
                 "  memory write   : addr 0x%016llx, size %zu bytes\n"
                 "  cache track    : 0x%016llx%016llx\n"
                 "  decoder track  : 0x%016llx%016llx\n",
                 static_cast<unsigned long long>(report.mem_addr),
                 report.mem_size,
                 static_cast<unsigned long long>(report.mem_fast_hi),
                 static_cast<unsigned long long>(report.mem_fast_lo),
                 static_cast<unsigned long long>(report.mem_ref_hi),
                 static_cast<unsigned long long>(report.mem_ref_lo));
  }
  std::abort();
}

}  // namespace

// Runs the two tracks in lockstep over one shared stack buffer. `fast_` has the
// cache (cache track); `ref_` has a null cache (decoder track only). After each
// instruction the full architectural state of the two is compared; the first
// disagreement becomes a DivergenceReport handed to the divergence handler.
class ShadowRunner::Impl {
 public:
  Impl(PredecodeCache* cache, void* stack_buffer, size_t stack_size)
      : fast_(cache, stack_buffer, stack_size),
        ref_(nullptr, stack_buffer, stack_size) {
    // Capture each step's memory writes into per-track lists; RunFrom clears
    // them before every step, so each list holds just that step's writes.
    fast_.SetMemoryWriteObserver([this](const Simulator::MemoryWriteEvent& w) {
      fast_writes_.push_back(w);
    });
    ref_.SetMemoryWriteObserver([this](const Simulator::MemoryWriteEvent& w) {
      ref_writes_.push_back(w);
    });
  }

  // Initial register state is mirrored to both Simulators so they start equal.
  void WriteXRegister(unsigned code, uint64_t value) {
    const GpRegister reg = static_cast<GpRegister>(code);
    fast_.Write(reg, value);
    ref_.Write(reg, value);
  }
  uint64_t ReadXRegister(unsigned code) const {
    return fast_.Read(static_cast<GpRegister>(code));
  }
  void WriteSp(uint64_t value) {
    fast_.Write(GpRegister::SP, value);
    ref_.Write(GpRegister::SP, value);
  }
  uint64_t ReadSp() const { return fast_.Read(GpRegister::SP); }

  void SetDivergenceHandler(DivergenceHandler handler) {
    handler_ = std::move(handler);
  }

  void SetBranchHook(Simulator::BranchHook hook, void* user_data) {
    fast_.SetBranchHook(hook, user_data);
    ref_.SetBranchHook(hook, user_data);
  }

  void RunFrom(uintptr_t entry_pc) {
    fast_.Write(GpRegister::PC, entry_pc);
    ref_.Write(GpRegister::PC, entry_pc);
    for (;;) {
      // At the loop top the two tracks agree (the previous step compared
      // equal, or this is the first step), so either PC is the instruction
      // about to run.
      const uintptr_t pc = static_cast<uintptr_t>(fast_.Read(GpRegister::PC));
      fast_writes_.clear();
      ref_writes_.clear();
      const bool fast_alive = fast_.StepOnce();
      const bool ref_alive = ref_.DebugStepOnce();
      if (!fast_alive && !ref_alive) {
        // Both tracks have terminated; nothing executed this round.
        break;
      }
      DivergenceReport report{};
      if (CompareStep(pc, &report)) {
        if (handler_) {
          handler_(report);
        } else {
          DefaultDivergenceHandler(report);
        }
        // The first divergence has been reported; stop the lockstep run. (If
        // the handler aborted, this is unreachable.)
        break;
      }
      if (!fast_alive || !ref_alive) {
        // One track terminated and the other did not. CompareStep already
        // caught this as a PC divergence; stop before stepping a finished
        // Simulator.
        break;
      }
    }
  }

 private:
  // Compare the two tracks' architectural state after one lockstep step.
  // Returns true and fills `*report` on the first differing item.
  bool CompareStep(uintptr_t pc, DivergenceReport* report) {
    // General-purpose registers X0..X30.
    for (unsigned i = 0; i <= 30; ++i) {
      const GpRegister reg = static_cast<GpRegister>(i);
      const uint64_t f = fast_.Read(reg);
      const uint64_t r = ref_.Read(reg);
      if (f != r) {
        char name[8];
        std::snprintf(name, sizeof(name), "X%u", i);
        FillRegReport(report, pc, static_cast<int>(i), name, f, 0, r, 0);
        return true;
      }
    }
    // Stack pointer.
    {
      const uint64_t f = fast_.Read(GpRegister::SP);
      const uint64_t r = ref_.Read(GpRegister::SP);
      if (f != r) {
        FillRegReport(report, pc, -1, "SP", f, 0, r, 0);
        return true;
      }
    }
    // FP/SIMD registers V0..V31, compared over their full 128 bits.
    for (unsigned i = 0; i <= 31; ++i) {
      const VRegister reg = static_cast<VRegister>(i);
      const VRegisterValue f = fast_.Read(reg);
      const VRegisterValue r = ref_.Read(reg);
      if ((f.lo != r.lo) || (f.hi != r.hi)) {
        char name[8];
        std::snprintf(name, sizeof(name), "V%u", i);
        FillRegReport(report,
                      pc,
                      static_cast<int>(i),
                      name,
                      f.lo,
                      f.hi,
                      r.lo,
                      r.hi);
        return true;
      }
    }
    // PC, flags, and control state.
    {
      const uint64_t fpc = fast_.Read(GpRegister::PC);
      const uint64_t rpc = ref_.Read(GpRegister::PC);
      if (fpc != rpc) {
        FillRegReport(report, pc, -1, "PC", fpc, 0, rpc, 0);
        return true;
      }
    }
    {
      const uint32_t f = fast_.Read(SysRegister::NZCV);
      const uint32_t r = ref_.Read(SysRegister::NZCV);
      if (f != r) {
        FillRegReport(report, pc, -1, "NZCV", f, 0, r, 0);
        return true;
      }
    }
    {
      const uint32_t f = fast_.Read(SysRegister::FPCR);
      const uint32_t r = ref_.Read(SysRegister::FPCR);
      if (f != r) {
        FillRegReport(report, pc, -1, "FPCR", f, 0, r, 0);
        return true;
      }
    }
    {
      const uint32_t f = fast_.Read(SysRegister::FPSR);
      const uint32_t r = ref_.Read(SysRegister::FPSR);
      if (f != r) {
        FillRegReport(report, pc, -1, "FPSR", f, 0, r, 0);
        return true;
      }
    }
    {
      const uint32_t f = fast_.Read(SysRegister::BType);
      const uint32_t r = ref_.Read(SysRegister::BType);
      if (f != r) {
        FillRegReport(report, pc, -1, "BType", f, 0, r, 0);
        return true;
      }
    }
    // Memory writes performed by this instruction.
    return CompareMemoryWrites(pc, report);
  }

  bool CompareMemoryWrites(uintptr_t pc, DivergenceReport* report) const {
    const size_t n = (fast_writes_.size() > ref_writes_.size())
                         ? fast_writes_.size()
                         : ref_writes_.size();
    for (size_t i = 0; i < n; ++i) {
      const bool fast_has = i < fast_writes_.size();
      const bool ref_has = i < ref_writes_.size();
      const Simulator::MemoryWriteEvent fw =
          fast_has ? fast_writes_[i] : Simulator::MemoryWriteEvent{0, 0, 0, 0};
      const Simulator::MemoryWriteEvent rw =
          ref_has ? ref_writes_[i] : Simulator::MemoryWriteEvent{0, 0, 0, 0};
      if ((fw.address != rw.address) || (fw.size != rw.size) ||
          (fw.value_lo != rw.value_lo) || (fw.value_hi != rw.value_hi)) {
        report->kind = DivergenceReport::Kind::MemoryWrite;
        report->pc = pc;
        report->reg_index = -1;
        report->reg_name[0] = '\0';
        report->mem_addr = fast_has ? fw.address : rw.address;
        report->mem_size = fast_has ? fw.size : rw.size;
        report->mem_fast_lo = fw.value_lo;
        report->mem_fast_hi = fw.value_hi;
        report->mem_ref_lo = rw.value_lo;
        report->mem_ref_hi = rw.value_hi;
        return true;
      }
    }
    return false;
  }

  static void FillRegReport(DivergenceReport* report,
                            uintptr_t pc,
                            int reg_index,
                            const char* reg_name,
                            uint64_t fast_lo,
                            uint64_t fast_hi,
                            uint64_t ref_lo,
                            uint64_t ref_hi) {
    report->kind = DivergenceReport::Kind::Register;
    report->pc = pc;
    report->reg_index = reg_index;
    // Copy the name into the report's own inline storage: the report must stay
    // valid after this ShadowRunner is gone and after the next divergence is
    // formatted, so it cannot hold a pointer into runner-owned memory.
    std::snprintf(report->reg_name, sizeof(report->reg_name), "%s", reg_name);
    report->fast_lo = fast_lo;
    report->fast_hi = fast_hi;
    report->ref_lo = ref_lo;
    report->ref_hi = ref_hi;
  }

  Simulator fast_;
  Simulator ref_;
  std::vector<Simulator::MemoryWriteEvent> fast_writes_;
  std::vector<Simulator::MemoryWriteEvent> ref_writes_;
  DivergenceHandler handler_;  // empty -> DefaultDivergenceHandler
};


ShadowRunner::ShadowRunner(PredecodeCache* cache,
                           void* stack_buffer,
                           size_t stack_size)
    : impl_(std::make_unique<Impl>(cache, stack_buffer, stack_size)) {}

ShadowRunner::~ShadowRunner() = default;

void ShadowRunner::WriteXRegister(unsigned code, uint64_t value) {
  impl_->WriteXRegister(code, value);
}

uint64_t ShadowRunner::ReadXRegister(unsigned code) const {
  return impl_->ReadXRegister(code);
}

void ShadowRunner::WriteSp(uint64_t value) { impl_->WriteSp(value); }

uint64_t ShadowRunner::ReadSp() const { return impl_->ReadSp(); }

void ShadowRunner::RunFrom(uintptr_t entry_pc) { impl_->RunFrom(entry_pc); }

void ShadowRunner::SetDivergenceHandler(DivergenceHandler handler) {
  impl_->SetDivergenceHandler(std::move(handler));
}

void ShadowRunner::SetBranchHook(Simulator::BranchHook hook, void* user_data) {
  // The two inner Simulators each own their own hook pointer; install the
  // same one on both so a lockstep run sees identical hook firings.
  impl_->SetBranchHook(hook, user_data);
}

}  // namespace testing
}  // namespace gaby_vm
