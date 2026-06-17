# PredecodeCache: support data-in-stream (don't reject a range for embedded read-only data)

Status: **proposed / not yet implemented.** Ref doc for a future session. Found
by the `vixl_port` review (issue I8). The code change is small; the reason it is
deferred is that it reverses a *deliberate, documented* design decision in
shipping code (the optimization's core data structure), so it needs its own
OpenSpec change with the rationale reconciled — not a drive-by edit.

## The defect (real, not cosmetic)

Real AArch64 code routinely interleaves **read-only data with instructions** in
the same executable range: literal pools (`LDR x, =imm` / `LDR x, label`), jump
tables, and inline constants. The bytes are data; the code branches *over* them
and only ever *loads* from them — the PC never executes a data word.

`PredecodeCache::RegisterCodeRange` predecodes **every** 4-byte word in the
range as if it were an instruction. When a data word decodes to an unallocated
encoding (very common for arbitrary 64-bit constants split into two words), the
predecode pass **rejects the whole range** — so none of the surrounding *real*
code becomes cache-executable either. In the `vixl_port` guard rail this shows
up as 5 skipped cases; in production it means a perfectly normal function with a
literal pool cannot use the cache track at all.

This is exactly the input shape the predecode/dispatch optimization must handle,
so the limitation is on the critical path of the project's main goal.

## Empirical ground truth (measured 2026-06-10)

A temporary probe in the harness (`GetLastRegistrationError` on rejection)
showed **all** rejected `vixl_port` cases hit one path:

```
status=1 (InvalidArgument)  reason="range contains an unallocated instruction encoding"
  AARCH64_ASM_ldr_literal
  AARCH64_ASM_ldr_literal_custom
  AARCH64_ASM_ldr_literal_custom_shared
  AARCH64_ASM_ldr_literal_range
  AARCH64_ASM_fjcvtzs           (fp family)
```

None hit `UnsupportedFeature`. The auditor is constructed with
`CPUFeatures::All()` (`predecode_cache.cc` `Impl()`), so feature rejection is
rare; the real blocker is the **unallocated** check.

## Root cause (one place)

`Sources/gaby_vm/src/gaby_vm/predecode_cache.cc`, the predecode loop:

```cpp
if (!auditor_.InstructionIsAvailable()) {           // ~line 259
  ...
  return RegistrationStatus::UnsupportedFeature;     // (a) not observed, but same class
}
if (capture_visitor_.unallocated()) {                // ~line 270  <-- the blocker
  SetError(RegistrationStatus::InvalidArgument, insn_addr,
           "range contains an unallocated instruction encoding", "");
  return RegistrationStatus::InvalidArgument;
}
const uint32_t form_hash = capture_visitor_.form_hash();
const void* leaf = vixl::aarch64::Simulator::ResolvePredecodeLeaf(form_hash);
if (leaf == nullptr) {
  leaf = UnimplementedSentinelLeaf();                // <-- the mechanism we want to reuse
}
```

The mechanism to reuse already exists: a **named-but-unimplemented** form is not
rejected — it gets `UnimplementedSentinelLeaf()` (`VisitUnimplemented`, which
aborts if ever executed) and the range registers `Ok`.

## Proposed fix (the approach from the review discussion)

Treat an **unallocated** word (and, by the same logic, an
**unsupported-feature** word) the way an unimplemented form is already treated:
give it a sentinel entry and keep predecoding, instead of failing the range.

```cpp
if (capture_visitor_.unallocated()) {
  entries[i] = { /*form_hash=*/0, /*flags=*/0, DataInStreamSentinelLeaf() };
  continue;   // not executable, but registerable
}
```

Why this is correct and safe:

- **The cache never alters the underlying bytes.** A `LDR`-literal reads from the
  real host address through the simulator's memory path, not from the
  `PredecodedEntry`. So loads of the data continue to see the original bytes —
  exactly the property the review asked to preserve.
- **The data word's entry is never dispatched.** Well-formed code branches over
  its literal pool; the PC never lands on a data word. The sentinel entry just
  occupies its slot in the flat `entries[]` array (preserving the
  `(pc-start)/4` indexing).
- **If the PC *does* land on a data word, that is a real bug** (a wild jump), and
  the sentinel traps it — strictly better than today, where the range was never
  cached so the bug would surface differently (or not on the cache track at all).

This is the same trap-on-execute model the design already uses for
unimplemented forms; it unifies "can't really execute this word" handling.

## The deliberate decision this reverses (must be reconciled)

- **design.md R12** (`docs/refs/gaby-vm-predecode-cache-design.md:670`):
  > Unallocated -> registration fails with `ErrorDetail` pointing at the PC.
  > Unimplemented -> `unimplemented_thunk`; aborts when PC reaches that word.

  i.e. the designer *intentionally split* unallocated (reject) from unimplemented
  (sentinel). This proposal merges them. The new session must update R12 with the
  data-in-stream rationale.

- **all-or-nothing rationale** (`...design.md:299`): the all-or-nothing story is
  about **UnsupportedFeature** in the iOS-dylib case — "let the embedder learn
  which PC needs which feature so it can re-try or drop the dylib." That rationale
  is about *feature probing*, a different concern from *unallocated data words*.
  Relaxing unallocated does not necessarily mean relaxing UnsupportedFeature; the
  new session should decide each independently (see open questions).

## Spec impact (smaller than it looks)

`openspec/specs/predecode-cache/spec.md` ("RegisterCodeRange validates inputs and
reports failures all-or-nothing") enumerates the normative non-`Ok` results:
`OverlappingRange`, `UnsupportedFeature`, `InvalidArgument` **for size only**
("size is zero or not a multiple of 4"), `OutOfMemory`. It does **not** mandate
rejecting an unallocated encoding — the `unallocated → InvalidArgument` behaviour
is implementation detail beyond the spec (and arguably a spec/impl mismatch
already). So the OpenSpec change mainly needs to:

1. Add a requirement/scenario: *a range with embedded non-instruction data
   registers `Ok`; its data words are non-executable sentinels; loads of the data
   read the original bytes; a PC that reaches a data word traps.*
2. Reconcile design.md R12.
3. Decide UnsupportedFeature's fate (keep its scenario, or relax it too).

## Open questions for the new session

1. **Scope: unallocated only, or also UnsupportedFeature?** The 5 observed cases
   are all unallocated. UnsupportedFeature has a live spec scenario and a real
   iOS-dylib rationale (feature probing). Recommend: relax **unallocated**
   (clearly data); leave UnsupportedFeature **as-is** for now (or add an opt-in),
   since its rejection carries embedder-facing diagnostic value.
2. **Distinct sentinel or reuse `UnimplementedSentinelLeaf`?** A distinct
   `DataInStreamSentinelLeaf` could emit a clearer "PC entered a data region"
   abort message than `VisitUnimplemented`. Low cost; recommend distinct.
3. **Opt-in strict mode?** Some embedders may *want* up-front rejection of
   undecodable ranges. Consider a per-cache or per-call flag if anyone needs it
   (YAGNI until then).
4. **Existing cache unit tests** almost certainly assert `unallocated →
   InvalidArgument`. They must be updated to the new contract.

## Test plan

- **PredecodeCache unit test** (new, in the cache's own test): register a range =
  `[real insns][64-bit data constant][more real insns][ret]` where the data word
  is a known unallocated encoding. Assert: `RegisterCodeRange == Ok`; running the
  cache track over the code executes correctly and the `LDR`-literal reads the
  data bytes; a deliberate branch *into* the data word traps via the sentinel.
- **Differential/absolute**: the 5 `vixl_port` cases auto-revive (they will now
  register `Ok` and run on the cache track). Confirm each passes both oracles
  (cache == decoder == reference). Verify they don't merely stop-skipping but
  actually pass.
- **Rebaseline** `kFamilyBaselines` (expect integer +4, fp +1; **measure**, do not
  assume — run with `VIXL_PORT_REBASELINE=1`). Update both debug and release
  pairs and the tasks/spec counts.
- **Guard rail**: `ctest --test-dir build/debug -R vixl_port` and the release
  preset green before/after.

## Estimated effort

Code: ~10–20 lines in `predecode_cache.cc` + a new sentinel + a unit test.
Surrounding work (OpenSpec change, design.md R12 reconciliation, spec
requirement, cache-test updates, harness rebaseline) is the bulk. One focused
session. **Do it as its own OpenSpec change**, not folded into the vixl_port
review-fix branch.
