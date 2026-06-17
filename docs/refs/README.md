# docs/refs Reference Index

[Chinese version](README.zh-cn.md)

This directory holds long-lived reference material for gaby-vm: VIXL background,
current design documents, and benchmark or profile snapshots that current code
or specs still cite. One-off historical snapshots and superseded drafts belong
in [`../archive/`](../archive/).

## How to Use This Directory

- Start with the categories below. Each entry says what the file is for and who
  still references it.
- Do not move or rename files marked **anchor**. Shipping code comments or
  OpenSpec specs cite those paths, sometimes down to section names. Put new
  historical snapshots under [`../archive/`](../archive/) instead of growing
  this directory indefinitely.
- If an old path no longer exists, search by filename. Filenames are stable, but
  old docs are periodically archived and we do not maintain every historical
  path reference.
- English is the default documentation language. When a Chinese translation
  exists, it sits next to the English file as `.zh-cn.md`.

## VIXL Background

Contributor-facing VIXL primers and factual references.

- `vixl-overview.md`: high-level VIXL project overview.
- `vixl-aarch64-simulator-architecture.md`: AArch64 simulator architecture,
  including registers and execution model.
- `vixl-decode-dispatch-pattern.md`: factual reference for per-instruction
  decode and dispatch control flow.
- `vixl-fetch-decode-dispatch-deep-dive.md`: fetch/decode/dispatch deep dive
  and the motivation for the predecode cache.

## Import Boundary

- `vixl-extraction-map.md`: **anchor**. VIXL import tier list, Tier 0 boundary,
  and pinned import SHA. `AGENTS.md`, `architecture.md`, `testing.md`,
  `onboarding.md`, and CMake files cite it.

## Current gaby-vm Design

- `gaby-vm-predecode-cache-design.md`: **anchor**. Authoritative design for the
  predecode/dispatch cache. Shipping comments in `predecode_cache.h` and
  `simulator-aarch64.h` cite specific sections.
- `gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md`: design for the
  current `vixl_port` suite: live assembly plus two-mode differential testing.
  `testing.md` cites it.
- `gaby-vm-predecode-cache-data-in-stream-2026-06-10.md`: design note for
  data-in-stream support. `testing.md` and the related OpenSpec change cite it.

## Baselines and Profile Snapshots Still in Use

These are point-in-time snapshots, but current code, docs, or specs still cite
them.

- `baseline-benchmark-suite.md`: benchmark methodology and suite index, cited
  by the OpenSpec `benchmark-harness` spec.
- `gaby-vm-business-bench-2026-06-08.md`: first `bench_business` results and
  interpretation, cited by `bench/README.md`.
- `gaby-vm-business-bench-applogic-2026-06-09.md`: FP/NEON `applogic` addition
  to the business benchmark, cited by `bench/README.md`.
- `gaby-vm-cache-hotpath-profile-2026-05-27.md`: **anchor**. Cache hot-path
  sampling profile, cited by shipping code comments and the
  `aarch64-simulator` spec.
- `gaby-vm-dispatch-flatten-profile-2026-06-11.md`: business-kernel cache-path
  dispatch profile and design analysis. Covers second dispatch, virtual-call
  cost, measured MOVPRFX rate, operand-storage trade-offs, and recovering
  32 bits after dropping `form_hash` from the flattened path.

## Archive

Historical and superseded documents move to [`../archive/`](../archive/):

- `archive/benchmarks/`: early baseline result snapshots.
- `archive/profiles/`: early cache hot-path profiles.
- `archive/superseded/`: superseded designs, plans, process notes, and early
  sketches.

Old path references, especially under `openspec/changes/archive/`, are not
updated one by one. Search by filename when a historical link goes stale.
