## Why

The `aarch64-simulator` capability spec, authored alongside the original VIXL
import (`extract-vixl-aarch64-simulator-baseline`), declared the imported
simulator structurally frozen:

- "The `Decoder → VisitNamedInstruction → leaf` dispatch flow SHALL be invoked
  unchanged."
- "No predecode cache, IR, or alternative dispatch path SHALL be introduced by
  this change."

That language was correct *for the import change itself* — the import shipped a
faithful replica of upstream and proved it could decode + execute single
instructions, nothing more. As a *normative* clause on the capability,
however, it now blocks the project's stated main optimization direction
(`predecode once → cache decoded dispatch target → execute cached path
repeatedly`, per `CLAUDE.md`).

The predecode/decode cache designed in
[`docs/refs/gaby-vm-predecode-cache-design.md`](../../../docs/refs/gaby-vm-predecode-cache-design.md)
needs to:

- Add new methods, fields, and helper classes to imported `Simulator`
  (`ExecuteInstructionCached`, `StepOnce`, `MemoryWriteSink`, `write_sink_`,
  etc.) — each bracketed by a `// gaby-vm:` marker comment referencing the
  design doc.
- Make the runtime hot path go through the cache instead of the imported
  decoder, while keeping the imported decoder reachable for diagnostic /
  tracing / shadow-self-test paths (the `DebugRunFrom` API in the design).

Both are blocked by the current spec wording. The marker convention
(byte-identical-outside-markers, license headers preserved, single grep over
`gaby-vm` finds every drift) is the auditability mechanism the project relies
on; this change keeps that convention untouched. What it relaxes is the
extra structural-freeze clause that was scoped to the import change but
written into the forever spec.

## What Changes

- **MODIFIED requirement** *Imported simulator code is preserved structurally*:
  - The bullet "Dispatch flow SHALL be invoked unchanged" → "Dispatch flow
    SHALL remain reachable" (alternate dispatch paths are permitted alongside,
    each marker-bracketed and referencing a design document).
  - The bullet "No predecode cache, IR, or alternative dispatch path SHALL be
    introduced by this change" → REMOVED. The clause was scoped to the import
    change; as a forever rule it directly contradicts the project's stated
    optimization direction.
  - A new bullet making it explicit that *additive* structural changes (new
    methods, fields, helper classes) ARE permitted when they follow the marker
    convention and the marker reason names the design document that motivates
    them. *Removing* or *renaming* existing members remains forbidden.
  - A new bullet pinning leaf-semantic preservation as the load-bearing
    invariant (replaces the old "dispatch unchanged" with the more precise
    "leaf semantics unchanged"; `VisitXxx`, `Simulate_*`, `LogicAArch64`
    helpers).
- The corresponding scenario *No predecode cache is introduced* is replaced
  by *Imported dispatch flow remains reachable* — same auditable property,
  but stated as "reachable" rather than "exclusive".

Unchanged:

- The marker convention itself (single-line `// gaby-vm:` and multi-line
  `// gaby-vm BEGIN` / `// gaby-vm END`).
- "Imported files are byte-identical to upstream except at marked locations".
- License headers, `LICENSE.vixl`, attribution.
- Tier-bounded import scope, layout mirroring, namespace preservation.
- Warning-policy boundary, VIXL preprocessor define scoping, public-header
  surface restrictions.

## Capabilities

### New Capabilities

*(none — this change relaxes one existing requirement of an existing capability)*

### Modified Capabilities

- `aarch64-simulator`: relax the *Imported simulator code is preserved
  structurally* requirement so that alternate dispatch paths (predecode cache)
  may be introduced alongside the imported flow, without weakening the marker
  convention.

## Impact

- **Spec edit**: one requirement modified (one bullet rewritten, one bullet
  removed, two bullets added; one scenario rewritten). No requirement added or
  removed at the requirement level.
- **No source code change** in this change. The cache, shadow runner, and
  associated marker-bracketed edits to `src/aarch64/simulator-aarch64.{h,cc}`
  will land in a separate `predecode-cache-implementation` change after this
  spec relaxation is in.
- **Documents updated**: `openspec/specs/aarch64-simulator/spec.md` (live
  spec).
- **Compatibility**: existing imports (Tier 1+2+3 baseline, with no markers)
  remain compliant — they trivially satisfy the relaxed requirement. The
  marker convention they currently don't trigger continues to apply when the
  cache work lands.
- **Cross-doc**: the predecode cache design doc
  (`docs/refs/gaby-vm-predecode-cache-design.md`) §2.3 already declares it
  depends on this relaxation; that dependency is now resolved.
