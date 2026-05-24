## Context

`predecode-cache-core` delivers the predecode cache and the public
`gaby_vm::Simulator` / `gaby_vm::PredecodeCache` API. Its design decision D5
deliberately scoped *benchmarking* out into this change — because the cache
has hard, falsifiable correctness criteria while the benchmark has only a
**soft** one (a meaningful improvement, no committed `N×`), and bundling the
two muddies what "done" means. The motivation and the soft-acceptance posture
are in `docs/refs/gaby-vm-predecode-cache-design.md` §4.5.

The `benchmark-harness` capability today
(`openspec/specs/benchmark-harness/spec.md`) runs a fixed workload through the
imported `vixl::aarch64::Simulator` and reports throughput. It mandates
exactly two binaries (`bench_baseline`, `bench_smoke`), a fixed
workload-header schema, a warm-up + steady-state timing loop, a key/value
reporting contract — and it currently forbids the harness from consuming any
new public API surface.

## Goals / Non-Goals

**Goals:**

- Make cache-on throughput measurable against the decoder baseline on the
  existing workloads, with the smallest possible disturbance to the
  `benchmark-harness` contract.
- Keep cache-on and cache-off runs directly comparable — same workloads, same
  warm-up and timing protocol, same output shape.

**Non-Goals:**

- Changing the cache, the `gaby_vm` public API, or any imported file.
- New workloads, microbenchmarks, or a hard performance target.
- CI / continuous-benchmarking automation — benchmarks stay developer-invoked.

## Decisions

### D1. An engine selector on the existing binaries, not new binaries

**Decision.** The shared `bench/` runner gains an engine selector —
`--engine {decoder|cache}`, default `decoder`. `bench_baseline` and
`bench_smoke` remain the only two benchmark executables; each runs either
engine.

**Why.** The `benchmark-harness` spec mandates "exactly two executables". A
runtime flag keeps that true and keeps the workload schema, timing protocol,
and reporting contract intact — the only spec-level change is that the harness
may now consume the `gaby_vm` public cache API, which the current spec forbids
("introduces no new public API surface"). One requirement modified, one added.

**Alternatives.** Add `bench_baseline_cached` / `bench_smoke_cached` binaries —
rejected: it doubles the binary count against an explicit "exactly two"
requirement and needs a larger spec rewrite for no measurement benefit.

### D2. Depends on, and archives after, `predecode-cache-core`

**Decision.** The cache engine consumes `gaby_vm::PredecodeCache` and
`gaby_vm::Simulator` — public types `predecode-cache-core` introduces. This
change is implemented and archived *after* core. Its spec delta validates
independently of core's archive state (validation checks delta structure and,
for the MODIFIED requirement, a header match against the *current* live
`benchmark-harness` spec — which already exists), so the two changes can be
authored in parallel.

**Why.** The seam is the *finalized public* `gaby_vm` API. By the time this
change is implemented, core has delivered and frozen that API, so there is no
shared internal surface to coordinate. The `benchmark-harness` MODIFIED
requirement names the `predecode-cache` capability, which becomes a live spec
when core archives — hence the archive ordering.

### D3. Reuse the committed workloads; register the workload buffer as a code range outside the timed region

**Decision.** The `mixed` / `smoke` workload headers are reused unchanged. For
the cache engine, the runner constructs a `PredecodeCache` and calls
`RegisterCodeRange` over the workload's instruction buffer **before** the
warm-up call. The decoder engine path is untouched.

**Why.** `RegisterCodeRange` runs the one-time predecode pass; doing it before
the warm-up keeps populate cost out of the timed steady-state region, so the
measurement reflects cached *execution*, not cache *construction*. The
existing single discarded warm-up iteration then suffices for both engines —
no engine-specific timing logic, so cache-on and cache-off numbers stay
directly comparable.

## Risks / Trade-offs

- **Engine selection adds a branch to the shared runner.** → Keep it a thin
  branch at *setup* time (which simulator to build, whether to register a
  cache); the steady-state timing loop stays uniform across engines, so the
  measured region is identical in shape.
- **This change cannot archive before `predecode-cache-core`.** → Stated as a
  dependency (D2); validation is independent, so authoring is not blocked.
- **A cache-on run that is not faster would be surprising.** → That is the
  *point* of measuring; a non-improvement is a finding for a follow-up, not a
  reason to withhold the harness. Acceptance is soft by design.

## Migration Plan

Nothing to migrate — the engine selector is an additive flag with a
backward-compatible default (`decoder` = today's behavior). On archive,
`openspec archive` applies the `benchmark-harness` deltas (one requirement
modified, one added) to the live spec. Rollback is a `git revert`; the change
touches only `bench/`.

## Open Questions

*(none — D1–D3 settle the harness shape; the cache API surface this change
consumes is fixed by `predecode-cache-core`.)*
