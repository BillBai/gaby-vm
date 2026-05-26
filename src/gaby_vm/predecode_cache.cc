// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#include "gaby_vm/predecode_cache.h"

#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <utility>

#include "cpu-features.h"
#include "globals-vixl.h"
#include "utils-vixl.h"

#include "aarch64/cpu-features-auditor-aarch64.h"
#include "aarch64/decoder-aarch64.h"
#include "aarch64/instructions-aarch64.h"
#include "aarch64/simulator-aarch64.h"

namespace gaby_vm {
namespace {

// File-local aliases for the PredecodeCache nested types so the
// implementation reads with one-word names. Anonymous-namespace scope keeps
// these names from leaking past this translation unit.
using RegistrationStatus = PredecodeCache::RegistrationStatus;
using RegistrationError = PredecodeCache::RegistrationError;
using PredecodedEntry = PredecodeCache::PredecodedEntry;
using CodeRange = PredecodeCache::CodeRange;

// The type a PredecodedEntry::leaf opaque handle really points at. It must
// match vixl::aarch64::Simulator's private FormToVisitorFnMap::mapped_type:
// the predecode pass stores a pointer to one of these (a process-lifetime
// pointer-to-member-function — either an entry in VIXL's form->leaf map, or
// the unimplemented sentinel below), and ExecuteInstructionCached casts the
// opaque handle back to this exact type before calling it as
// (this->*pmf)(pc_). Keep the alias byte-compatible with Simulator's
// mapped_type — D1 of predecode-cache-hotpath-speedup changed it from
// std::function to a raw pmf to remove the type-erasure indirection.
using LeafFn = void (vixl::aarch64::Simulator::*)(
    const vixl::aarch64::Instruction*);

// Sentinel leaf for an instruction whose encoding the VIXL decoder names but
// for which the Simulator carries no leaf — an "unimplemented" form (design
// doc R12). Registering a range that contains such an instruction still
// succeeds; the abort fires only if the cache track actually reaches it.
//
// The imported Simulator::VisitUnimplemented already prints the address and
// encoding and aborts via VIXL_UNIMPLEMENTED; using its address directly keeps
// the sentinel byte-compatible with every other entry in
// Simulator::FormToVisitorFnMap (each of which is a pmf) and removes the
// duplicate std::function-wrapped lambda this file used to carry.
//
// Held as a function-local static so the pmf has process lifetime — the
// PredecodedEntry::leaf opaque handle points straight at it.
const LeafFn* UnimplementedSentinelLeaf() {
  static const LeafFn sentinel =
      &vixl::aarch64::Simulator::VisitUnimplemented;
  return &sentinel;
}

// Predecode-time BTI-relevance classifier, written to bit 0 of
// PredecodedEntry::flags so the cache-track hot path can skip the
// BType / guarded-page check on instructions whose outcome does not interact
// with BType. The set of relevant forms — BTI, PACI[AB]SP, BRK, HLT, and the
// exception-causing forms (SVC/HVC/SMC/DCPS1-3) — is exactly the set that the
// imported runtime check in Simulator::ExecuteInstructionCached inspects, so
// the classification mirrors that check via VIXL's existing
// Instruction accessors:
//   - IsBti()        — BTI, BTI_c, BTI_j, BTI_jc
//   - IsPAuth()      — PACIASP/PACIBSP (and other PAuth forms; the runtime
//                       check then narrows further on SystemPAuthMask)
//   - IsException()  — BRK, HLT, SVC, HVC, SMC, DCPS1-3
// The form_hash parameter is currently unused but kept on the signature for
// the future per-form predecode work the `flags` slot is reserved for
// (predecode-cache-hotpath-speedup design.md D2/D3).
bool IsBtiRelevant(uint32_t /*form_hash*/,
                   const vixl::aarch64::Instruction* insn) {
  return insn->IsBti() || insn->IsPAuth() || insn->IsException();
}

// Decoder visitor used only by the predecode pass. For each decoded
// instruction it records the form hash and whether the encoding is
// unallocated, reading both off the Metadata the decoder hands every visitor.
// It is registered on the cache's predecode decoder after the
// CPUFeaturesAuditor, so a single decoder->Decode() call both audits the
// instruction and captures its form.
class FormCaptureVisitor : public vixl::aarch64::DecoderVisitor {
 public:
  void Visit(vixl::aarch64::Metadata* metadata,
             const vixl::aarch64::Instruction* /*instr*/) override {
    unallocated_ = metadata->count("unallocated") > 0;
    // VIXL guarantees every Metadata carries a "form" entry, including for an
    // unallocated encoding.
    form_hash_ = vixl::Hash((*metadata)["form"].c_str());
  }

  bool unallocated() const { return unallocated_; }
  uint32_t form_hash() const { return form_hash_; }

 private:
  bool unallocated_ = false;
  uint32_t form_hash_ = 0;
};

// One registered range. The CodeRange (the public, lookup-facing view) and the
// entry-array storage that backs it live together in one std::map node, so a
// registered range is exactly one allocation and the CodeRange a Simulator
// caches in cur_range_ is stable for the cache's lifetime (design.md D9).
struct RangeRecord {
  CodeRange info{0, 0, nullptr};
  std::unique_ptr<PredecodedEntry[]> storage;
};

}  // namespace

class PredecodeCache::Impl {
 public:
  Impl() : auditor_(&decoder_, vixl::CPUFeatures::All()) {
    decoder_.AppendVisitor(&capture_visitor_);
  }

  ~Impl() { decoder_.RemoveVisitor(&capture_visitor_); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  RegistrationStatus RegisterCodeRange(const void* start, size_t size_bytes);

  // Slow-path PC -> range lookup: takes the table's shared lock and binary-
  // searches the start-sorted range map. The per-instruction fast path (a hit
  // in the Simulator's own cur_range_) never reaches here.
  const CodeRange* FindRange(uintptr_t pc) const {
    std::shared_lock<std::shared_mutex> lock(table_mutex_);
    if (ranges_.empty()) {
      return nullptr;
    }
    // upper_bound gives the first range whose start is strictly above pc; the
    // range that could contain pc is therefore its predecessor.
    std::map<uintptr_t, RangeRecord>::const_iterator it =
        ranges_.upper_bound(pc);
    if (it == ranges_.begin()) {
      return nullptr;
    }
    --it;
    const CodeRange& info = it->second.info;
    if ((pc - info.start) < info.size_bytes) {
      return &info;
    }
    return nullptr;
  }

  const RegistrationError* last_error() const { return &last_error_; }

 private:
  // Record a failed-registration diagnostic. `reason` must be a static string.
  // `missing` is copied into cache-owned storage so RegistrationError can hand
  // back a const char* that stays valid until the next failed call.
  void SetError(RegistrationStatus status,
                uintptr_t pc,
                const char* reason,
                std::string missing) {
    missing_features_ = std::move(missing);
    last_error_.status = status;
    last_error_.pc = pc;
    last_error_.reason = reason;
    last_error_.missing_features = missing_features_.c_str();
  }

  // The predecode pass reuses one decoder for every registration; the
  // exclusive table lock held by RegisterCodeRange serialises access to it.
  vixl::aarch64::Decoder decoder_;
  vixl::aarch64::CPUFeaturesAuditor auditor_;
  FormCaptureVisitor capture_visitor_;

  // Single-writer / multiple-reader guard over the range map: RegisterCodeRange
  // takes it exclusively, FindRange takes it shared (design.md D9).
  mutable std::shared_mutex table_mutex_;
  std::map<uintptr_t, RangeRecord> ranges_;

  RegistrationError last_error_{RegistrationStatus::Ok, 0, "", ""};
  std::string missing_features_;
};


RegistrationStatus PredecodeCache::Impl::RegisterCodeRange(const void* start,
                                                           size_t size_bytes) {
  const unsigned kInsnSize = vixl::aarch64::kInstructionSize;
  const uintptr_t start_addr = reinterpret_cast<uintptr_t>(start);

  // The exclusive lock is taken up front, before input validation. A rejected
  // registration still writes the diagnostic state (last_error_ /
  // missing_features_) through SetError, so even the validation failure paths
  // mutate shared state — leaving them outside the lock would let two threads
  // registering bad ranges race on last_error_. Under the lock, the cost is
  // only the cheap argument arithmetic below. The lock then goes on to cover
  // the overlap check, the predecode pass (which uses the shared decoder) and
  // the commit, so registrations are fully serialised and no FindRange reader
  // ever sees a half-built table.
  std::unique_lock<std::shared_mutex> lock(table_mutex_);

  // --- input validation ---
  if ((size_bytes == 0) || ((size_bytes % kInsnSize) != 0)) {
    SetError(RegistrationStatus::InvalidArgument,
             0,
             "size must be a non-zero multiple of the 4-byte instruction size",
             "");
    return RegistrationStatus::InvalidArgument;
  }
  if ((start_addr % kInsnSize) != 0) {
    SetError(RegistrationStatus::InvalidArgument,
             start_addr,
             "start address must be 4-byte (instruction) aligned",
             "");
    return RegistrationStatus::InvalidArgument;
  }

  // --- overlap check: any overlap with an existing range is rejected ---
  const uintptr_t end_addr = start_addr + size_bytes;
  for (const auto& kv : ranges_) {
    const CodeRange& r = kv.second.info;
    if ((start_addr < (r.start + r.size_bytes)) && (r.start < end_addr)) {
      SetError(RegistrationStatus::OverlappingRange,
               start_addr,
               "range overlaps an already-registered range",
               "");
      return RegistrationStatus::OverlappingRange;
    }
  }

  // --- allocate the predecoded-entry array ---
  const size_t num_entries = size_bytes / kInsnSize;
  std::unique_ptr<PredecodedEntry[]> entries;
  try {
    entries = std::make_unique<PredecodedEntry[]>(num_entries);
  } catch (const std::bad_alloc&) {
    SetError(RegistrationStatus::OutOfMemory,
             start_addr,
             "could not allocate predecoded-entry storage for the range",
             "");
    return RegistrationStatus::OutOfMemory;
  }

  // --- predecode pass: decode every word, audit it, resolve its leaf ---
  for (size_t i = 0; i < num_entries; ++i) {
    const uintptr_t insn_addr = start_addr + (i * kInsnSize);
    const vixl::aarch64::Instruction* insn =
        reinterpret_cast<const vixl::aarch64::Instruction*>(insn_addr);
    // Drives the auditor and the form-capture visitor in one pass.
    decoder_.Decode(insn);

    if (!auditor_.InstructionIsAvailable()) {
      std::ostringstream oss;
      oss << auditor_.GetInstructionFeatures();
      SetError(RegistrationStatus::UnsupportedFeature,
               insn_addr,
               "instruction requires a CPU feature the cache auditor rejects",
               oss.str());
      // `entries` is freed here; the range map is untouched — nothing
      // registered (all-or-nothing).
      return RegistrationStatus::UnsupportedFeature;
    }
    if (capture_visitor_.unallocated()) {
      SetError(RegistrationStatus::InvalidArgument,
               insn_addr,
               "range contains an unallocated instruction encoding",
               "");
      return RegistrationStatus::InvalidArgument;
    }

    const uint32_t form_hash = capture_visitor_.form_hash();
    const void* leaf =
        vixl::aarch64::Simulator::ResolvePredecodeLeaf(form_hash);
    if (leaf == nullptr) {
      // A named-but-unimplemented form: registerable, but aborts if executed.
      leaf = UnimplementedSentinelLeaf();
    }
    entries[i].form_hash = form_hash;
    // Bit 0 of `flags` marks the entry as BTI-relevant so the cache-track
    // hot path can elide the BType / guarded-page check on forms whose
    // outcome doesn't interact with BType. Remaining bits stay zero —
    // future per-form predecode work (operand pre-extraction, etc.) lands
    // in this slot. See predecode-cache-hotpath-speedup design.md D3/D4.
    entries[i].flags = IsBtiRelevant(form_hash, insn) ? 1u : 0u;
    entries[i].leaf = leaf;
  }

  // --- commit: the first and only mutation of the range table ---
  // emplace is the lone remaining allocation; the moves and assignments after
  // it are noexcept, so a failure here still leaves nothing registered.
  try {
    RangeRecord& record =
        ranges_.emplace(start_addr, RangeRecord{}).first->second;
    record.storage = std::move(entries);
    record.info = CodeRange{start_addr, size_bytes, record.storage.get()};
  } catch (const std::bad_alloc&) {
    SetError(RegistrationStatus::OutOfMemory,
             start_addr,
             "could not allocate a range-table node",
             "");
    return RegistrationStatus::OutOfMemory;
  }
  return RegistrationStatus::Ok;
}


PredecodeCache::PredecodeCache() : impl_(std::make_unique<Impl>()) {}

PredecodeCache::~PredecodeCache() = default;

RegistrationStatus PredecodeCache::RegisterCodeRange(const void* start,
                                                     size_t size_bytes) {
  return impl_->RegisterCodeRange(start, size_bytes);
}

const RegistrationError* PredecodeCache::GetLastRegistrationError() const {
  return impl_->last_error();
}

const CodeRange* PredecodeCache::FindRange(uintptr_t pc) const {
  return impl_->FindRange(pc);
}

}  // namespace gaby_vm
