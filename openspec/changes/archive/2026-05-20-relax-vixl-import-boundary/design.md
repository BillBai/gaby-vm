## Context

This is a spec-only change. There is no implementation step — the change *is*
the spec edit.

The motivating context lives in
[`docs/refs/gaby-vm-predecode-cache-design.md`](../../../docs/refs/gaby-vm-predecode-cache-design.md):
the cache implementation will need to add `ExecuteInstructionCached`,
`StepOnce`, `MemoryWriteSink` (a small interface class), and a
`write_sink_` field to imported `Simulator`, plus split the runtime hot path
between cache (default for `RunFrom`) and imported decoder (default for
`DebugRunFrom`). All additions are marker-bracketed, but the *structural*
additions and the *runtime path split* both run afoul of the existing
"preserved structurally" requirement.

Two earlier sister documents inform the relaxation:

- `gaby-vm-modification-sketch.md` (in `docs/refs/`) is the project-wide design
  sketch where predecode cache as the main optimization direction is declared.
- `vixl-fetch-decode-dispatch-deep-dive.md` is the cycle / risk analysis that
  motivates `ExecuteInstruction` being on the cache fast path rather than the
  imported decoder.

## Goals / Non-Goals

**Goals**

- Allow the cache implementation to land via the marker convention without a
  spec-rules conflict.
- Preserve the auditability the marker convention provides (byte-identical
  outside markers, single grep finds every drift).
- Make the load-bearing invariant precise: leaf semantics are preserved,
  marker comments name a design doc, alternate paths coexist with the imported
  flow.

**Non-Goals**

- Relaxing the byte-identical-outside-markers rule. The marker convention is
  the project's auditability mechanism; this change keeps it.
- Relaxing the license-header, attribution, or tier-import requirements.
- Relaxing the public-header restriction (no `vixl::*` in `include/gaby_vm/`).
- Pre-approving any specific cache-implementation patch. The relaxation is
  generic — any future change still has to follow the marker convention and
  pass review.

## Decisions

### D1. Modify, do not split, the existing requirement

**Decision**. The "Imported simulator code is preserved structurally"
requirement is modified in place rather than replaced by a new pair (e.g.,
"structurally preserved" + "alternate dispatch permitted"). The single
requirement, after this change, captures what *is* still preserved (leaf
semantics, namespace, removals forbidden) and what *is* permitted (additive
markers, alternate paths coexisting with the imported flow).

**Why**. Splitting into two adjacent requirements would create a "what changed
where" navigation problem; modifying one keeps the spec readable. The
requirement's title remains accurate — structural preservation is *still* the
load-bearing rule, just with a more precise definition of "structural".

**Alternatives**

- Add a new requirement "Predecode cache integration is permitted" alongside
  the existing one. Rejected: requires reading both side-by-side to know
  what's actually allowed; awkward.
- Remove the existing requirement entirely. Rejected: loses the leaf-semantic
  + namespace + no-removal guarantees that *are* still load-bearing.

### D2. Replace `SHALL be invoked unchanged` with `SHALL remain reachable`

**Decision**. The bullet about the imported dispatch flow becomes:

> The imported `Decoder → VisitNamedInstruction → leaf` dispatch flow SHALL
> remain reachable from the simulator. A non-cached execution path (e.g.,
> for tracing, debugging, shadow self-test, or bring-up) SHALL be available
> that exercises this flow.

**Why**. "reachable" is the precise property the cache design needs:
`DebugRunFrom` keeps the imported flow live for diagnostic and shadow
self-test purposes; that path is the spec's contractual guarantee that we
haven't quietly broken the imported flow. "invoked unchanged" implied
"on every execution", which the cache fast path explicitly violates.

### D3. Remove `No predecode cache ... by this change` rather than rephrase

**Decision**. The bullet "No predecode cache, IR, or alternative dispatch
path SHALL be introduced by this change" is removed entirely.

**Why**. The clause was scoped to a specific past change ("by this change");
as a forever rule it has no meaning, and as a forever rule against alternate
dispatch it directly contradicts the project's optimization direction.
Rephrasing ("predecode cache is permitted") is captured by the new bullet
about alternate paths under D1, so leaving the negative clause in would be
redundant noise.

### D4. Make additive permission explicit in the requirement body

**Decision**. Add a bullet to the requirement body:

> Structural additions (new methods, fields, helper classes) on imported
> types ARE permitted when they follow the marker convention AND when each
> marker reason names the design document that motivates them. Removing or
> renaming an existing member is NOT permitted.

**Why**. The pre-existing scenario "Imported `Simulator` class declaration
matches upstream" already says additions are permitted inside marker regions,
but that's a scenario, not the load-bearing rule itself. Promoting it to the
requirement body makes the permission explicit and pairs it with the design-doc
naming requirement, which is the *new* discipline this change adds. Reviewers
checking marker comments now have a spec-grounded reason to require a doc
reference, not just "what" but "why".

### D5. New scenario: imported flow reachability

**Decision**. The old scenario "No predecode cache is introduced" is replaced
by:

> #### Scenario: Imported dispatch flow remains reachable
> - **WHEN** the imported `Decoder → VisitNamedInstruction → leaf` flow is
>   exercised through any non-cached execution path (e.g., a `DebugRunFrom`
>   entry, a tracing loop, or any path that goes through `Decoder::Decode`)
> - **THEN** the simulator reaches the imported flow without going through
>   an alternate dispatch
> - **AND** alternative dispatch paths (predecode cache, etc.) MAY exist
>   alongside, each introduced with marker comments naming the design
>   document

**Why**. The original scenario tested a negative ("dispatch goes through
the imported flow, no alternate"). The new scenario tests the positive
auditable property ("imported flow is reachable") plus an explicit allowance
("alternate paths may exist alongside"). The "MAY exist alongside" wording
is the spec-level statement that the cache implementation is now in scope.

## Risks / Trade-offs

- **Loosening might invite ad-hoc edits.** The relaxation explicitly requires
  marker reasons to name a design document, which is a new — and stricter —
  discipline than the pre-relaxation convention. PR reviewers should reject
  marker comments that don't reference a doc.
- **`SHALL remain reachable` can be gamed.** Someone could technically
  satisfy "reachable" with a dead-code `if (false)` path. Mitigation: the
  scenario's `DebugRunFrom`-or-equivalent wording requires a *callable*
  entry point, not just code that compiles.
- **Spec drift**. As the cache lands, the spec may need additional
  requirements (e.g., "ShadowRunner exists", "`RunFrom` aborts on
  out-of-range PC"). Those belong to the predecode-cache change, not this
  one. This change strictly opens the door; it does not specify what walks
  through it.

## Migration Plan

There is nothing to migrate. The Tier 1+2+3 baseline import currently has no
markers, so it trivially satisfies the relaxed requirement. The next change
(`predecode-cache-implementation`) will add the first markers — that change
will be the first test of the new wording, including the design-doc-naming
discipline. Rollback is `git revert` of the archive commit; the in-flight
proposal is in `openspec/changes/archive/` for historical reference.

## Open Questions

*(none — the relaxation is mechanical; the disputable part was decided in
the predecode-cache-design brainstorm)*
