## Context

`PredecodeCache::RegisterCodeRange` (`Sources/gaby_vm/src/gaby_vm/predecode_cache.cc`)
predecodes every 4-byte word in a range as an instruction. Its loop rejects the
whole range when a word's encoding is `unallocated`, and (in a branch that is
currently dead — see below) when the auditor reports the word needs an
unavailable CPU feature. Real code embeds read-only data (literal pools, jump
tables, inline constants) in executable ranges, so this all-or-nothing reject
locks such functions out of the cache track. The mechanism to fix it already
exists: a named-but-unimplemented form is *not* rejected — it gets
`UnimplementedSentinelLeaf()` and the range registers `Ok` (design.md R12).

Empirical ground truth (ref doc, measured 2026-06-10): all rejected `vixl_port`
cases hit the **unallocated** path (`ldr_literal*`, `fjcvtzs`); none hit
`UnsupportedFeature`. Cause: the cache auditor is built with
`CPUFeatures::All()`, and VIXL's `All()` sets every feature bit, so
`CPUFeaturesAuditor::InstructionIsAvailable()` always returns true — the
`UnsupportedFeature` reject branch is **unreachable** today.

Full analysis and the deliberate decision being reversed:
[ref doc](../../../docs/refs/gaby-vm-predecode-cache-data-in-stream-2026-06-10.md),
design.md R12 (`docs/refs/gaby-vm-predecode-cache-design.md`).

## Goals / Non-Goals

**Goals:**
- A range with embedded read-only data registers `Ok` and runs on the cache track.
- A data word never silently executes: reaching one traps with its address.
- Loads of the data read the original bytes (the cache never alters them).
- One unified, consistent contract: only structural problems reject a range.

**Non-Goals:**
- No new IR, no JIT, no change to imported leaf semantics.
- No opt-in "strict / reject undecodable ranges" mode (YAGNI until an embedder needs it).
- No change to the `CPUFeatures::All()` auditor configuration.
- No public-header (`predecode_cache.h`) change; the enum keeps all its values.

## Decisions

**D1 — Sentinel-and-continue instead of reject (the fix).** In the predecode
loop, a word that does not decode to an executable cached instruction is written
as a sentinel entry (`form_hash=0`, `flags=0`, sentinel leaf) and the loop
continues, instead of returning a failure. This mirrors the existing
unimplemented-form handling and preserves the flat `(pc-start)/4` indexing (the
sentinel just occupies its slot). *Alternative:* partial registration that marks
data words specially in a side table — rejected; more state, no benefit, the
flat array already gives us a free slot.

**D2 — Reuse `Simulator::VisitUnallocated` as the data sentinel.** The ref doc's
open question weighed "reuse `UnimplementedSentinelLeaf` vs. write a new
`DataInStreamSentinelLeaf` with a clearer message." The imported
`Simulator::VisitUnallocated` already prints `Unallocated instruction at
<addr>: <bits>` and aborts via `VIXL_UNIMPLEMENTED` — semantically accurate for
the common (unallocated) case, *distinct* from `VisitUnimplemented`, and
byte-compatible with every other `FormToVisitorFnMap` pmf. So the new
`DataInStreamSentinelLeaf()` helper points at `&Simulator::VisitUnallocated` —
a distinct, accurate sentinel at **zero edits to imported code**, beating both
options the ref doc considered. *Alternative:* add a gaby-authored
`VisitGabyDataInStream` member to the imported `Simulator` (marker convention)
for a bespoke "PC entered a data region" message — rejected as unnecessary
imported-code churn; the address + abort already locate the bug, and the
decoder track lands on the same `VisitUnallocated` for a wild PC, so the two
tracks stay aligned.

**D3 — Relax both `unallocated` and `!InstructionIsAvailable()` (scope).** Both
route to the data sentinel; only structural checks (size, overlap, OOM) reject.
Rationale: the bytes are data — *why* they fail to decode (unallocated vs.
feature-gated) does not change that, so a single rule is cleaner than splitting
them. Note the honest nuance: under the `All()` auditor the feature branch is
unreachable, so this half is **forward-looking** (it only takes effect if a
future change makes the auditor stricter) and is *not* exercised by a runtime
test — the unallocated half is the reachable, TDD-tested behavior change.
*Alternative:* relax unallocated only and keep `UnsupportedFeature` (the ref
doc's first recommendation) — rejected by the maintainer: a data word that
happened to be feature-gated should not reject a range any more than an
unallocated one should, and keeping a dead, inconsistent reject branch is worse
than one uniform rule.

**D4 — Keep the `UnsupportedFeature` enum value (ABI).** `RegistrationStatus` is
`enum class : int`, documented C-ABI stable. Removing `UnsupportedFeature`
(value 3) would renumber `OutOfMemory`. The value is retained but no longer
produced; `RegistrationError.missing_features` likewise stays in the struct,
unpopulated. *Alternative:* delete the value — rejected (ABI break for an
append-only enum).

## Risks / Trade-offs

- **Loss of up-front feature probing** (the design.md:299 iOS-dylib rationale:
  reject so the embedder learns which PC needs which feature) → Mitigation:
  largely theoretical — the path is already dead under `All()`. If an embedder
  ever wants strict feature rejection, that returns as an explicit opt-in (a
  future change), not as today's silent-because-dead branch.
- **A genuinely corrupt instruction stream (real code that is unallocated) now
  registers `Ok` instead of failing at registration** → Mitigation: strictly
  better behavior. Today the range is simply not cached; after this change a PC
  that reaches the bad word aborts with its address via `VisitUnallocated` —
  exactly what the decoder track already does, so both tracks now agree.
- **A data word that decodes to a *valid, implemented* form gets a real leaf**
  (unchanged from today) → not a regression: well-formed code never lands on it,
  and it was already registerable before this change. Only the *reject* paths
  move to the sentinel.

## Migration Plan

1. TDD the reachable behavior: a failing cache unit test (embedded unallocated
   word → `Ok`, cache-track load reads the bytes, branch-into-data aborts), then
   the `predecode_cache.cc` change.
2. Reconcile docs: update design.md R12 and the §4.3.2 all-or-nothing /
   `UnsupportedFeature` rationale to record the data-in-stream decision.
3. Rebaseline `kFamilyBaselines` in the `vixl_port` harness with
   `VIXL_PORT_REBASELINE=1` (measure the revived counts; do not assume), update
   both debug and release pairs.
4. Guard rail: `ctest -R vixl_port` green under `dev-debug` and `dev-release`,
   before and after.

Rollback: revert the `predecode_cache.cc` change and the baseline counts; no
data migration, no persisted state.

## Open Questions

None outstanding — the four open questions from the ref doc are resolved above
(scope = both, per D3; sentinel = reuse `VisitUnallocated`, per D2; no strict
mode, per Non-Goals; no existing unit test asserts `unallocated → InvalidArgument`,
verified, so none to update).
