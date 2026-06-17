# Tasks

Ordering rule: every `ci/` script is validated by running it locally on the
arm64 host against a real build before it is wired into a workflow. Workflows
are added only after the scripts they call are green locally. See
[design.md](./design.md) for the script contract and decisions.

## 1. Scaffold + shared helpers

- [ ] 1.1 Create `ci/` with `ci/util.sh` (sourced, not executed): repo-root
      locate, binary locate (`libgaby_vm.a`, `bench_*`, `native_*`,
      `size_probe`), human size formatter, markdown-table emitter, JSON writer,
      `summary_append` (append to `$GITHUB_STEP_SUMMARY` when set, else stdout).
- [ ] 1.2 Verify: `bash -n ci/util.sh` parses clean; a throwaway
      `source ci/util.sh && <each helper>` call produces expected output on
      stdout (no `$GITHUB_STEP_SUMMARY` set ⇒ falls back to stdout).

## 2. Build + test track

- [ ] 2.1 Write `ci/build.sh <preset> [extra -D…]`: `cmake --preset <preset>`
      with passed-through `-D` flags, then `cmake --build --preset <preset>`.
      Non-zero exit on any cmake failure (`set -euo pipefail`).
- [ ] 2.2 Write `ci/ctest.sh <preset>`: run the full suite
      (`ctest --test-dir build/<preset-dir> --output-on-failure`), capture
      pass/fail counts + total time, emit a markdown result line, propagate the
      ctest exit code.
- [ ] 2.3 Verify locally: `ci/build.sh dev-debug` then `ci/ctest.sh dev-debug`
      reports 22/22 pass and exits 0; the emitted markdown names the failing
      test on an induced failure (temporarily break one assert to confirm
      non-zero exit, then revert).

## 3. Parity gate

- [ ] 3.1 Write `ci/bench-verify.sh <preset>`: build-aware locate of
      `bench_business`, run `--verify` (all kernels), parse the final
      `verify: OK` / mismatch line, emit a markdown result, exit non-zero on
      any mismatch or missing binary.
- [ ] 3.2 Verify locally: against `dev-release`, `ci/bench-verify.sh dev-release`
      prints OK for all five kernels and exits 0.

## 4. Size report + `size_probe`

- [ ] 4.1 Add a minimal `size_probe` target under `bench/` (behind
      `GABY_VM_BUILD_BENCHMARKS`): a `.cc` that constructs a `Simulator`, runs a
      couple of instructions through the public API, returns. Link with
      dead-strip; do not change `Sources/gaby_vm/src/CMakeLists.txt`.
- [ ] 4.2 Write `ci/size-report.sh [--base sizes.json]`: measure raw
      `libgaby_vm.a` (`stat`) and the stripped `size_probe` (`strip` a copy,
      `size -m` → `__TEXT`+`__DATA`); write `sizes.json`; emit a markdown table;
      when `--base` is given, add a delta column; when absent, render
      "no base to compare". Never exits non-zero on size alone.
- [ ] 4.3 Verify locally: `ci/build.sh dev-release -DGABY_VM_BUILD_BENCHMARKS=ON`
      builds `size_probe`; `ci/size-report.sh` emits a `sizes.json` with both
      numbers and a markdown table; re-running with `--base sizes.json` shows a
      `0` delta.

## 5. Full bench report

- [ ] 5.1 Write `ci/bench-report.sh`: drive the whole harness — `native_business`
      / `bench_business --mode decoder` / `--mode cache` (per-kernel three-way),
      `native_baseline` / `bench_baseline` both modes (mixed workload), and
      `bench_smoke` as a pipeline sanity check; assemble `bench.json` +
      a markdown three-way table. Report-only (always exit 0 unless a binary is
      missing or a run crashes).
- [ ] 5.2 Verify locally: with a `dev-release` build that has
      `-DGABY_VM_BUILD_NATIVE_BASELINE=ON`, `ci/bench-report.sh --seconds 0.5`
      produces a populated table for all five kernels + the mixed baseline and a
      well-formed `bench.json`.

## 6. PR/push workflow (`ci.yml`)

- [ ] 6.1 Write `.github/workflows/ci.yml`: `on` pull_request + push to `main`;
      `runs-on: macos-14`; `concurrency` cancel-in-progress keyed on ref;
      `permissions: { contents: read, pull-requests: write }`. One job:
      checkout → `build.sh dev-debug` → `ctest.sh dev-debug` →
      `build.sh dev-release -DGABY_VM_BUILD_BENCHMARKS=ON` →
      `bench-verify.sh dev-release` → fetch latest `main` `sizes.json` via
      `gh run download` (best-effort) → `size-report.sh --base …` →
      append report to `$GITHUB_STEP_SUMMARY` → on PR, sticky-comment via
      `ci/util.sh` → upload `sizes.json` artifact.
- [ ] 6.2 Verify: `actionlint` (or `gh workflow view` once pushed) reports no
      syntax/expression errors; the job step order matches the script contract;
      the sticky-comment step is guarded to PR events only.

## 7. Bench workflow (`bench.yml`)

- [ ] 7.1 Write `.github/workflows/bench.yml`: `on` push to `main` +
      `workflow_dispatch`; `runs-on: macos-14`; no PR trigger; default token.
      Job: checkout → `build.sh dev-release -DGABY_VM_BUILD_BENCHMARKS=ON
      -DGABY_VM_BUILD_NATIVE_BASELINE=ON` → `bench-report.sh` → append to job
      summary → upload `bench.json` artifact.
- [ ] 7.2 Verify: `actionlint` clean; confirm there is no `pull_request`
      trigger and no perf threshold that could fail the run.

## 8. CI reference doc

- [ ] 8.1 Write `docs/refs/ci.md` (English, per the docs language policy):
      triggers, the two workflows, the `ci/` script table, how to reproduce each
      step locally, how to read the job-summary/PR-comment report, and the
      macOS-now / Linux-later note. Link it from `docs/refs/README.md` and the
      `AGENTS.md` references list.

## 9. Final verification

- [ ] 9.1 Run the full local dry-run end to end: `build.sh dev-debug` +
      `ctest.sh dev-debug` (22/22) + `build.sh dev-release …` +
      `bench-verify.sh` (OK) + `size-report.sh` + `bench-report.sh`, each exit
      0, each emitting its report. Capture the output as evidence.
- [ ] 9.2 `openspec validate add-github-ci --strict` passes.
- [ ] 9.3 Push the branch and confirm both workflows run green on GitHub
      Actions (the only step that needs the real runner); paste the run links.
