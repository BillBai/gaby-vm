## Why

The project has no automated CI. Every correctness and performance check
(`ctest`, the `vixl_port` guard rail, `bench_business --verify`, the bench
harness, binary size) is run by hand. A clean, green base is easy to drift
off of, and regressions in the cache hot path or in binary footprint are
caught only when someone remembers to look. We want GitHub Actions to run the
checks we already have, on every change, and report the results — without
adding a flaky performance gate to pull requests.

## What Changes

- Add **portable shell scripts under `ci/`** that wrap the existing build,
  test, parity, size, and benchmark steps. CI YAML only orchestrates them;
  a developer runs the same scripts locally. (See [design.md](./design.md).)
- Add **`.github/workflows/ci.yml`** (pull request + push to `main`,
  `macos-14` arm64): build, run the full CTest suite, run the bit-exact
  `bench_business --verify` parity check, and produce a binary-size report
  with deltas. The PR can fail only on a build error, a CTest failure, or a
  parity mismatch — all deterministic. No timing-based gate.
- Add **`.github/workflows/bench.yml`** (push to `main` + manual dispatch,
  `macos-14` arm64): run the full benchmark harness (the five business
  microkernels three-way against the native baseline, plus the mixed VIXL
  workload) and publish the numbers as a moving baseline artifact.
  Report-only.
- Add a small **`size_probe`** CMake target (behind the existing benchmarks
  option): a minimal public-API link unit so the size report can measure the
  stripped footprint an embedder actually pays, not just the unstripped
  archive.
- Reports go to the **GitHub job summary** on every run and to a **sticky PR
  comment** on pull requests.
- Add a short CI reference doc under `docs/refs/`.

## Capabilities

### New Capabilities

- `continuous-integration`: the normative CI contract — deterministic PR gate
  (build + full tests + cache/decoder parity), performance tracked on `main`
  as a moving baseline (never gating a PR), binary size reported with deltas
  (report-only), all CI logic reproducible locally via `ci/` scripts, and
  reports delivered to the job summary and a sticky PR comment.

### Modified Capabilities

(none)

## Impact

- New: `ci/` scripts (`util.sh`, `build.sh`, `ctest.sh`, `bench-verify.sh`,
  `size-report.sh`, `bench-report.sh`), `.github/workflows/{ci,bench}.yml`,
  the `size_probe` bench target, `docs/refs/ci.md`.
- The macOS arm64 runner is the only place the slowdown-vs-native figure is
  meaningful (GitHub Linux runners are x86_64); a Linux build/test job is a
  deliberate later follow-up, out of scope here.
- No change to shipping `Sources/gaby_vm/src/`, to imported VIXL semantics, or
  to the bench harness behaviour. CI runs what already exists.

## Non-goals

- No Linux/Android/HarmonyOS runner yet (later follow-up).
- No performance threshold that fails a PR. Perf is report-only.
- No size threshold that fails a PR. Size is report-only.
- No new benchmark, test, or kernel; no change to leaf semantics.
- No release/publish/signing pipeline; this is build/test/bench/size only.
