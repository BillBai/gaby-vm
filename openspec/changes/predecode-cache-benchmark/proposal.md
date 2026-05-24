## Why

`predecode-cache-core` builds the predecode cache and the dual-track
`gaby_vm::Simulator`, but — by that change's design decision D5 —
deliberately leaves *measurement* out: its acceptance is hard correctness
only. The predecode cache exists for dispatch speed, so the V1 program still
owes a measured answer to "is the cached path actually faster?"

The `baseline-benchmark-harness` change (archived 2026-05-14) anticipated
exactly this — it explicitly deferred the "cache-on vs cache-off comparison"
to "the change that introduces the cache." This is that follow-up: it teaches
the existing `bench/` harness to drive the cache, so the soft speed criterion
of `gaby-vm-predecode-cache-design.md` §4.5 can be verified.

## What Changes

- The `bench/` runner gains an **engine selector** — `--engine {decoder|cache}`,
  defaulting to `decoder` (today's behavior). The `cache` engine builds a
  `gaby_vm::PredecodeCache`, registers the workload's instruction buffer as a
  code range, and drives `gaby_vm::Simulator::RunFrom`.
- `bench_baseline` and `bench_smoke` stay the **only two** binaries — the
  engine is a runtime flag, not a new target.
- The key/value output gains an `engine` key so cache-on and cache-off runs
  are distinguishable.
- `bench/README.md` documents the flag and the cache-on vs cache-off
  comparison procedure.

### Non-Goals

- Any change to the cache itself, the `gaby_vm` public API, `src/`, imported
  VIXL files, or `include/gaby_vm/`. This change only *consumes* the public
  API that `predecode-cache-core` delivers.
- A committed `N×` target. Acceptance stays **soft** — a meaningful, measured
  improvement, consistent with `gaby-vm-predecode-cache-design.md` §4.5.
- New workloads. The committed `mixed` / `smoke` workloads are reused as-is.

## Capabilities

### New Capabilities

*(none — this change modifies one existing capability)*

### Modified Capabilities

- `benchmark-harness`: the harness may now consume the `gaby_vm` public cache
  API and select a cache execution engine, so cache-on throughput is
  measurable against the decoder baseline. The two-binary shape, the
  workload-header schema, the warm-up + steady-state timing protocol, and the
  reporting contract are all unchanged.

## Impact

- **Depends on `predecode-cache-core`.** It consumes `gaby_vm::PredecodeCache`
  and `gaby_vm::Simulator`; it is implemented and archived *after* that
  change. Its spec delta validates independently, so it can be authored in
  parallel.
- **Files**: `bench/runner.{h,cc}`, `bench/CMakeLists.txt`, `bench/README.md`.
- **No new public API**, and no edits under `src/`, `include/gaby_vm/`, or the
  imported VIXL tree.
- **Build**: the bench runner already links `gaby_vm`; it now also includes
  the public cache headers (`predecode_cache.h`, `simulator.h`).
