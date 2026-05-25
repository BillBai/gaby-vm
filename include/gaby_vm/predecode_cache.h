// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause

#ifndef GABY_VM_PREDECODE_CACHE_H_
#define GABY_VM_PREDECODE_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>

// The predecode / dispatch cache — gaby-vm's core execution optimisation.
//
// A PredecodeCache runs a one-time decode pass over each registered code range
// and records, for every 4-byte instruction word, the (form hash, leaf
// dispatcher) pair the imported VIXL decoder would otherwise recompute on every
// execution. The cache-track Simulator (see simulator.h) then dispatches an
// instruction with a single array load plus one indirect call, instead of the
// imported decoder's tree walk + visitor fan-out + form-hash map lookup.
//
// This header is part of the gaby-vm public API: it exposes no vixl:: type and
// includes no imported VIXL header, so an embedder sees only gaby_vm:: types.
// The authoritative design is docs/refs/gaby-vm-predecode-cache-design.md.

namespace gaby_vm {

// The predecode / dispatch cache.
//
// Lifetime is owned by the embedder. A single PredecodeCache may back several
// gaby_vm::Simulator instances at once — the predecoded data is shared and
// read-only after registration. RegisterCodeRange calls are serialised
// internally and are safe to interleave with Simulators executing
// already-registered ranges on other threads (the per-instruction fast path
// stays lock-free; only the rare cross-range lookup takes a shared lock). The
// cache is append-only: a registered range stays valid until the cache is
// destroyed, and V1 exposes no unregister / flush / invalidate operation.
class PredecodeCache {
 public:
  // Outcome of PredecodeCache::RegisterCodeRange.
  //
  // `enum class : int` so the underlying type is fixed and C-ABI stable: a C
  // embedder can treat the result as a plain int, and new variants may be
  // appended without disturbing the existing values.
  enum class RegistrationStatus : int {
    // The range was predecoded; its instructions now dispatch through the
    // cache.
    Ok = 0,
    // `size_bytes` was zero or not a multiple of the 4-byte instruction size.
    InvalidArgument,
    // The range overlaps an already-registered range — any overlap, including
    // an exact duplicate. The embedder owns de-duplication.
    OverlappingRange,
    // Some instruction in the range needs a CPU feature the cache's
    // CPUFeaturesAuditor does not accept. GetLastRegistrationError() locates
    // it.
    UnsupportedFeature,
    // Entry storage for the range could not be allocated.
    OutOfMemory,
  };

  // Diagnostic detail for the most recent *failed* RegisterCodeRange call.
  //
  // Plain-old-data by design: it holds no std::string and owns nothing.
  // `reason` and `missing_features` point at storage owned by the cache and
  // valid only until the next RegisterCodeRange call on the same cache.
  // Querying it is not thread-safe with respect to a concurrent
  // RegisterCodeRange — the caller must query on the same thread that made
  // the failed call. Mirrors the errno / strerror split (see design doc
  // 4.3.2): the status code stays simple, the multi-dimensional detail is
  // fetched separately.
  struct RegistrationError {
    // The failure this detail describes. `Ok` means no failure has been
    // recorded.
    RegistrationStatus status;
    // Host address of the offending instruction. Valid for
    // UnsupportedFeature; 0 otherwise.
    uintptr_t pc;
    // Static, human-readable summary of the failure. Never null.
    const char* reason;
    // For UnsupportedFeature: a printable list of the CPU feature(s) the
    // offending instruction needs but the auditor rejects. Empty string ("")
    // when not applicable. Never null.
    const char* missing_features;
  };

  // One predecoded instruction.
  //
  // Exactly 16 bytes so a registered code range maps to a flat entries[]
  // array indexed by (pc - range_start) / 4, making the cache-track lookup a
  // single array load. `leaf` is an opaque handle to the resolved VIXL leaf
  // dispatcher; only predecode_cache.cc (and the cache-track execution path)
  // interprets it, which keeps this public header free of any vixl:: type.
  // The V1 entry stores the (form_hash, leaf) pair directly — the per-form
  // thunk of the design doc 4.2.1 is deferred to V2. See design.md D8.
  struct PredecodedEntry {
    // VIXL form hash for this instruction. The cache-track execution path
    // writes it to Simulator::form_hash_ before invoking the leaf, because
    // shared Simulate_* leaves branch on form_hash_ (deep-dive R1).
    uint32_t form_hash;
    // Padding today; reserved for the V2 per-form operand pre-extraction slot
    // (design doc 4.6). Zero-initialised by the predecode pass.
    uint32_t reserved;
    // Opaque handle to the leaf dispatcher. predecode_cache.cc resolves and
    // interprets this; a null value never appears in a populated entry (an
    // unimplemented form is given a sentinel dispatcher, design doc R12).
    const void* leaf;
  };

  static_assert(sizeof(PredecodedEntry) == 16,
                "PredecodedEntry must stay 16 bytes for flat-array indexing");

  // A registered, predecoded code range.
  //
  // The embedder never constructs a CodeRange — it passes (start, size) to
  // RegisterCodeRange and the cache builds the record. The type is public
  // only so the cache-track execution path can perform its hot-path lookup
  // inline: given a CodeRange and a PC inside it, the entry is
  // &entries[(pc - start)/4].
  //
  // CodeRange records live in stable storage inside the cache and are never
  // relocated once created, so a pointer to one (a Simulator's cached
  // `cur_range_`) stays valid for the cache's lifetime. See design.md D9.
  struct CodeRange {
    // Host address of the range's first instruction word.
    uintptr_t start;
    // Size of the range in bytes; always a positive multiple of 4.
    size_t size_bytes;
    // Array of size_bytes/4 predecoded entries, one per instruction word.
    const PredecodedEntry* entries;
  };

  PredecodeCache();
  ~PredecodeCache();

  // Not copyable: the cache owns range storage shared by reference.
  PredecodeCache(const PredecodeCache&) = delete;
  PredecodeCache& operator=(const PredecodeCache&) = delete;

  // Predecode [start, start + size_bytes) so its instructions become
  // executable on a Simulator's cache track. `size_bytes` must be a positive
  // multiple of 4. May be called at any point in the cache's lifetime,
  // including after Simulators have been constructed and have executed.
  //
  // All-or-nothing: on any non-Ok result the cache is left exactly as it was —
  // no instruction from a failed call becomes cache-executable. On a non-Ok
  // result, GetLastRegistrationError() returns the corresponding detail.
  RegistrationStatus RegisterCodeRange(const void* start, size_t size_bytes);

  // Detail of the most recent failed RegisterCodeRange call on this cache.
  // Never null; `status` is Ok if no failure has been recorded. The pointee
  // is valid until the next RegisterCodeRange call (see RegistrationError).
  const RegistrationError* GetLastRegistrationError() const;

  // Find the registered range containing host address `pc`, or null if `pc`
  // lies in no registered range. This is the cache-track slow-path lookup:
  // it takes the range table's shared lock and runs a binary search. The
  // per-instruction fast path (a hit in the Simulator's cached range) does
  // not call this. Not part of the embedder-facing surface — it is the
  // integration seam used by the cache-track execution path.
  const CodeRange* FindRange(uintptr_t pc) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gaby_vm

#endif  // GABY_VM_PREDECODE_CACHE_H_
