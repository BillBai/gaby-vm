# Baseline Benchmark Results — 2026-05

> Pre-optimization throughput snapshot of the imported VIXL AArch64
> simulator running through gaby-vm's `bench/` harness. This is the
> follow-up report promised by
> [`baseline-benchmark-suite.md`](./baseline-benchmark-suite.md):
> methodology lives there, raw numbers live here.
>
> The path to gaby-vm's planned cached-dispatch design is sketched in
> [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md);
> these numbers are the floor that future work compares against. Once
> the predecode/dispatch cache lands, `bench_baseline` will no longer
> exercise the original upstream-shaped path on a hot codebase, so this
> document exists to preserve the time-zero data before that change.
>
> File name embeds the year/month on purpose: re-baselining lands as a
> sibling document (`baseline-benchmark-results-YYYY-MM.md`), not an
> in-place edit. Treat this file as immutable historical record.

## TL;DR

On an Apple M4 Pro performance core, single-threaded, with no host
hygiene beyond "moderately loaded laptop":

| Workload | Median throughput | Median ns/insn | Notes |
|----------|------------------:|---------------:|-------|
| `mixed`  | **7.46 M guest insn/s** | 134.0 | upstream `BenchCodeGenerator` (256 KiB buffer, ~64.6k dyn insn/iter) |
| `smoke`  | **12.47 M guest insn/s** | 80.2  | 32-instruction straight-line ALU body, no branches, no pool entries |

Order-of-magnitude framing:

- M4 Pro p-core native throughput is ~10^10 insn/s. Mixed is ~1340×
  slower than native; smoke is ~800× slower.
- Per-instruction cost translates to roughly **~600 host cycles per
  guest instruction** on `mixed`, **~360** on `smoke`, assuming ~4.5 GHz
  effective p-core frequency under a moderate boost.
- This is the upper end of what a "naive switch-case visitor" interpreter
  produces. The cached-dispatch design described in
  [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
  is the lever expected to move us into the 10^8 IPS band.

## Host

| Field | Value |
|------:|-------|
| Machine | MacBook (M4 Pro) |
| CPU | Apple M4 Pro, 10 P-cores + 4 E-cores |
| L1I per p-core | 192 KiB |
| L1D per p-core | 128 KiB |
| L2 (shared per p-core cluster) | 16 MiB |
| OS | macOS 26.3.1 (build 25D771280a) |
| Kernel | Darwin 25.3.0 arm64 |
| Compiler | Apple clang 21.0.0 (clang-2100.0.123.102) |
| Build flags | `CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `dev-release` preset |
| Power | AC, fan-cooled; not on battery |
| Concurrent load | `uptime` ~2.2; quiet shell, no media playback. **Not** a clean dedicated host. |

`mixed` writes 256 KiB of guest code, which fits in L2 but not in L1I.
`smoke` is 128 bytes, comfortably L1I-resident. The gap between the
two cases therefore conflates "branch + pool overhead" with "L1I
hit rate"; both effects favor `smoke`. See the methodology doc's
"Practical notes" section for the host hygiene we deliberately did
**not** apply (no core pinning, no governor tweaks, no turbo lock):
this snapshot is order-of-magnitude grade, not publication grade.

## Build provenance

- Repository commit: `e9a229742a4436f779ef15df5178683c024813d2`
  (`chore: add VSCode debug configs for lldb-dap`).
- Working tree at measurement time had two unstaged edits and two
  untracked files, all under `docs/refs/` — none affect compiled
  output. The benchmark binary used was built from the committed
  tree above.
- Binary: `build/release/bench/bench_baseline`,
  `build/release/bench/bench_smoke`. Both
  Mach-O 64-bit arm64.

## Workload provenance

Recorded by the bench binaries themselves, kept here for grep-ability:

- **mixed** — `workload_generator_tag: vixl@3fe168632164; seed=42; buffer_bytes=262192`,
  `static_words_in_buffer=65548`, `dynamic_instructions_per_iteration=64643`.
- **smoke** — `workload_generator_tag: llvm-mc 22.1.5; source_sha256=4769ba17a5fe`,
  `static_words_in_buffer=32`, `dynamic_instructions_per_iteration=32`
  (branch-free, so dynamic == static).

Both headers are committed to the repo; future runs against the same
headers measure the same exact instruction trace, modulo any future
edits to `Simulator::Visit*` semantics that would alter dynamic counts
on `mixed` (`smoke` is straight-line so its dynamic count cannot
drift).

## Raw observations

### `mixed` — 7 runs, `--seconds 5`

| Run | iterations/s | throughput (insn/s) | ns/insn |
|----:|-------------:|--------------------:|--------:|
| 1 | 115.79 | 7,485,056 | 133.60 |
| 2 | 115.41 | 7,460,515 | 134.04 |
| 3 | 116.19 | 7,511,061 | 133.14 |
| 4 | 114.37 | 7,392,947 | 135.26 |
| 5 | 114.71 | 7,415,282 | 134.86 |
| 6 | 114.10 | 7,375,968 | 135.58 |
| 7 | 116.00 | 7,498,850 | 133.35 |
| **median** | **115.41** | **7,460,515** | **134.04** |
| min | 114.10 | 7,375,968 | 133.14 |
| max | 116.19 | 7,511,061 | 135.58 |

Spread `(max − min) / median` ≈ **1.8 %** on throughput.

### `smoke` — 6 runs, `--seconds 1`

| Run | iterations/s | throughput (insn/s) | ns/insn |
|----:|-------------:|--------------------:|--------:|
| 1 | 382,849 | 12,251,187 | 81.62 |
| 2 | 392,469 | 12,559,012 | 79.62 |
| 3 | 396,614 | 12,691,649 | 78.79 |
| 4 | 390,731 | 12,503,397 | 79.98 |
| 5 | 386,959 | 12,382,715 | 80.76 |
| 6 | 388,924 | 12,445,581 | 80.35 |
| **median** | **389,828** | **12,474,489** | **80.16** |
| min | 382,849 | 12,251,187 | 78.79 |
| max | 396,614 | 12,691,649 | 81.62 |

Spread ≈ **1.6 %** on throughput.

The methodology doc asks for IQR and N=10 medians under pinned
governors — that protocol kicks in once we have a cache-on/cache-off
delta worth quantifying tightly. At the present 1–2 % run-to-run
spread on an unpinned host, deeper hygiene would change the trailing
digit and not the order of magnitude.

## What this measures, and what it does not

What the numbers reflect, accurately:

- The **upstream VIXL dispatch path**, end-to-end, on the same source
  imported into gaby-vm: `Simulator::RunFrom` →
  `Decoder::Decode(pc) → CompiledDecodeNode walk → DecoderVisitor
  virtual call → Simulator::VisitXXX leaf` per instruction. No
  predecode cache, no inline-threading, no register caching — the
  out-of-the-box upstream behavior.
- The cost includes the per-iteration `LR` reset done by the bench
  harness (`Simulator::WriteLr(kEndOfSimAddress)`), which is one
  register write per ~64.6k-instruction iteration on `mixed` and per
  32-instruction iteration on `smoke`. Cost amortizes to noise on
  `mixed`; it adds ~1 of the 80 ns on `smoke`. The methodology doc
  treats this as part of the "hot loop floor".
- Steady-state only: the harness runs one untimed warm-up `RunFrom`
  before the timed region, so first-touch instruction-cache effects
  are excluded from the reported numbers.

What the numbers do **not** capture:

- **Predecode cost.** Not yet a thing — there is no predecode pass in
  the present binary. Future result documents must add a separate
  "predecode (ms) for N instructions" column per the methodology.
- **Cache-on vs cache-off split.** Not applicable yet; no cache exists.
  The first cache implementation lands with a runtime toggle and
  reproduces this document's `mixed` and `smoke` rows on the cache-off
  side as a regression gate.
- **Multi-instance / atomic stress.** The
  [methodology doc lists this as the P0 correctness
  gate](./baseline-benchmark-suite.md#multi-instance-atomics-stress);
  it is not part of throughput baselining and is intentionally
  out of scope for this snapshot.
- **iOS numbers.** The bench harness runs on macOS only at this
  point. iPhone p-core performance scales roughly 0.85× of M4 Pro
  p-core throughput on similar workloads, so a rough estimate is
  ~6.3 M IPS on `mixed` / ~10.6 M IPS on `smoke` for an iPhone
  A18 Pro p-core; this is an extrapolation, not a measurement.

## How to reproduce

```sh
cmake --preset dev-release -DGABY_VM_BUILD_BENCHMARKS=ON
cmake --build --preset dev-release

# Repeat each command several times; take the median.
./build/release/bench/bench_baseline --seconds 5
./build/release/bench/bench_smoke    --seconds 1
```

The output format is `key: value` lines, documented in
[`bench/README.md`](../../bench/README.md). The primary metric is
`iterations_per_second`; `throughput_insn_per_sec` and
`ns_per_instruction` are derived.

## Reference points for future comparisons

When a new measurement campaign produces a sibling document, the
deltas worth tabulating against this snapshot are:

| Comparison | Expected magnitude (rough, not a target) |
|------------|------------------------------------------|
| Cache-off path vs this baseline | Within ±5 %. Larger drift is a regression signal in the upstream-shape path. |
| Cache-on `mixed` vs this `mixed` | 3×–10× speedup is the design intent of the predecode cache. |
| Cache-on `smoke` vs this `smoke` | Smaller speedup than `mixed`; smoke is already L1I-resident and dispatch-dominated, so the cache primarily removes decode work — expect ~2×–5×. |
| ns/insn floor under cache-on `smoke` | If we cannot push below ~30 ns/insn here, threaded-dispatch / register-caching is the next lever, not more cache surface. |

These are calibration anchors, not commitments.

## See also

- [`baseline-benchmark-suite.md`](./baseline-benchmark-suite.md) —
  methodology, metrics schema, multi-instance correctness gate.
- [`bench/README.md`](../../bench/README.md) — harness usage,
  output-key definitions, workload regeneration steps.
- [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
  — the predecode/dispatch cache design these numbers will be
  compared against.
- [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md)
  — what the ~134 ns/insn on `mixed` is actually spent on.
