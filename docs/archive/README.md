# docs/archive

[Chinese version](README.zh-cn.md)

This directory holds historical documents that no longer describe the current
state and are no longer referenced by live code or active docs. They are useful
for understanding old measurements and how designs changed, but day-to-day
work should start in [`../refs/`](../refs/). See
[`../refs/README.md`](../refs/README.md) for the sorting rules.

> Historical files such as `openspec/changes/archive/*` may still point at old
> `docs/refs/...` paths. Those links are not rewritten during archival. Search
> by filename; filenames stay stable even when paths move.

## benchmarks

Throughput snapshots taken at specific commits and superseded by newer data.

- `baseline-benchmark-results-2026-05.md`: earliest baseline result.
- `baseline-benchmark-results-cache-2026-05.md`: first cache-mode baseline.
- `baseline-benchmark-results-cache-2026-05-neon-inline.md`: snapshot after
  NEON inline work.
- `baseline-benchmark-results-cache-2026-05-clearforwrite-helpers.md`:
  snapshot after clear-for-write and helper work.

## profiles

Early or superseded cache hot-path profiles outside the still-live
`refs/gaby-vm-cache-hotpath-profile-2026-05-27.md` anchor.

- `gaby-vm-cache-hotpath-profile-2026-05.md`: first hot-path profile.
- `gaby-vm-cache-hotpath-profile-2026-06-02.md`: follow-up profile plus
  optimization roadmap.

## superseded

- `gaby-vm-vixl-sim-test-port-design-2026-06-08.md`: early frozen-fixture
  `vixl_port` design, replaced by live assembly.
- `gaby-vm-vixl-sim-test-port-plan-2026-06-08.md`: implementation plan for the
  frozen-fixture design, also superseded.
- `gaby-vm-vixl-port-live-assemble-review-2026-06-10.md`: code-review notes
  for the live-assemble rewrite.
- `gaby-vm-dispatch-redesign-notes-2026-06-02.md`: rough dispatch redesign
  notes.
- `gaby-vm-modification-sketch.md`: early overall modification sketch.

## Root

- `HANDOFF-predecode-cache-brainstorm.md`: early predecode-cache brainstorm
  handoff that already lived in this directory before this archive structure.
