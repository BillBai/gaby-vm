# Tasks

Ordering rule: every `ci/` script is validated by running it locally on the
arm64 host against a real build before it is wired into a workflow. Workflows
are added only after the scripts they call are green locally. See
[design.md](./design.md) for the script contract and decisions.

## 1. Scaffold + shared helpers

- [x] 1.1 Create `ci/` with `ci/util.sh` (sourced, not executed): repo-root
      locate, binary locate (`libgaby_vm.a`, `bench_*`, `native_*`,
      `size_probe`), human size formatter, markdown-table emitter, JSON writer,
      `summary_append` (append to `$GITHUB_STEP_SUMMARY` when set, else stdout).
      DONE: `ci/util.sh` — `ci_build_dir`, `ci_find`, `ci_human_kb`,
      `ci_file_bytes`, `ci_summary`, `ci_sticky_comment`. bash 3.2 compatible.
- [x] 1.2 Verify: `bash -n ci/util.sh` parses clean; a throwaway
      `source ci/util.sh && <each helper>` call produces expected output on
      stdout (no `$GITHUB_STEP_SUMMARY` set ⇒ falls back to stdout). DONE: all
      six scripts pass `bash -n`; helpers smoke-tested (build dirs, KB format,
      file bytes, summary→stdout).

## 2. Build + test track

- [x] 2.1 Write `ci/build.sh <preset> [extra -D…]`: `cmake --preset <preset>`
      with passed-through `-D` flags, then `cmake --build --preset <preset>`.
      Non-zero exit on any cmake failure (`set -euo pipefail`). DONE.
- [x] 2.2 Write `ci/ctest.sh <preset>`: run the full suite
      (`ctest --test-dir build/<preset-dir> --output-on-failure`), capture
      pass/fail counts + total time, emit a markdown result line, propagate the
      ctest exit code. DONE.
- [x] 2.3 Verify locally: `ci/build.sh dev-debug` then `ci/ctest.sh dev-debug`
      reports 22/22 pass and exits 0; the emitted markdown names the failing
      test on an induced failure. DONE: 22/22 pass, exit 0, markdown summary
      "✅ Tests — dev-debug".

## 3. Parity gate

- [x] 3.1 Write `ci/bench-verify.sh <preset>`: build-aware locate of
      `bench_business`, run `--verify` (all kernels), parse the final
      `verify: OK` / mismatch line, emit a markdown result, exit non-zero on
      any mismatch or missing binary. DONE.
- [x] 3.2 Verify locally: against `dev-release`, `ci/bench-verify.sh dev-release`
      prints OK for all five kernels and exits 0. DONE: "verify: OK (all
      selected kernels agree)", exit 0.

## 4. Size report + `size_probe`

- [x] 4.1 Add a minimal `size_probe` target under `bench/` (behind
      `GABY_VM_BUILD_BENCHMARKS`): a `.cc` that constructs a `Simulator`, runs a
      couple of instructions through the public API, returns. Link with
      dead-strip; do not change `Sources/gaby_vm/src/CMakeLists.txt`. DONE:
      `bench/size_probe.cc` (mov x0,#42 ; ret), links only `gaby_vm::gaby_vm`,
      `-Wl,-dead_strip` on Apple. `Sources/gaby_vm/src/CMakeLists.txt`
      untouched.
- [x] 4.2 Write `ci/size-report.sh [--base sizes.json]`: measure raw
      `libgaby_vm.a` (`stat`) and the stripped `size_probe`; write `sizes.json`;
      emit a markdown table; when `--base` is given, add a delta column; when
      absent, render "no base to compare". Never exits non-zero on size alone.
      DONE. (Metric refined from `size -m __TEXT+__DATA` to the stripped probe's
      file size — finer-grained and Linux-portable; the constant overhead
      cancels in the head-vs-base delta.)
- [x] 4.3 Verify locally: `ci/build.sh dev-release -DGABY_VM_BUILD_BENCHMARKS=ON`
      builds `size_probe`; `ci/size-report.sh` emits a `sizes.json` with both
      numbers and a markdown table; re-running with `--base sizes.json` shows a
      `0` delta. DONE: lib=3382.4 KB, stripped probe=1845.3 KB; `--base` self
      shows `0` / `0`.

## 5. Full bench report

- [x] 5.1 Write `ci/bench-report.sh`: drive the whole harness — `native_business`
      / `bench_business --mode decoder` / `--mode cache` (per-kernel three-way),
      `native_baseline` / `bench_baseline` both modes (mixed workload), and
      `bench_smoke` as a pipeline sanity check; assemble `bench.json` +
      a markdown three-way table. Report-only. DONE.
- [x] 5.2 Verify locally: with a `dev-release` build that has
      `-DGABY_VM_BUILD_NATIVE_BASELINE=ON`, `ci/bench-report.sh --seconds 0.5`
      produces a populated table for all five kernels + the mixed baseline and a
      well-formed `bench.json`. DONE: 5 kernels + `mixed` row, smoke "ok",
      valid `bench.json`.

## 6. PR/push workflow (`ci.yml`)

- [x] 6.1 Write `.github/workflows/ci.yml`: `on` pull_request + push to `main`;
      `runs-on: macos-14`; `concurrency` cancel-in-progress keyed on ref;
      `permissions: { contents: read, pull-requests: write }`. One job:
      checkout → `build.sh dev-debug` → `ctest.sh dev-debug` →
      `build.sh dev-release -DGABY_VM_BUILD_BENCHMARKS=ON` →
      `bench-verify.sh dev-release` → fetch latest `main` `sizes.json` via
      `gh run download` (best-effort) → `size-report.sh --base …` →
      append report to `$GITHUB_STEP_SUMMARY` → on PR, sticky-comment via
      `ci/util.sh` → upload `sizes.json` artifact. DONE. (PR number passed via
      `env:` per the workflow-injection guard.)
- [x] 6.2 Verify: `actionlint` reports no syntax/expression errors; the job
      step order matches the script contract; the sticky-comment step is guarded
      to PR events only. DONE: `actionlint` CLEAN (fixed one SC2086); the
      sticky-comment step is `if: github.event_name == 'pull_request'`.

## 7. Bench workflow (`bench.yml`)

- [x] 7.1 Write `.github/workflows/bench.yml`: `on` push to `main` +
      `workflow_dispatch`; `runs-on: macos-14`; no PR trigger; default token.
      Job: checkout → `build.sh dev-release … -DGABY_VM_BUILD_NATIVE_BASELINE=ON`
      → `bench-report.sh` → upload `bench.json` artifact. DONE.
- [x] 7.2 Verify: `actionlint` clean; confirm there is no `pull_request`
      trigger and no perf threshold that could fail the run. DONE: `actionlint`
      CLEAN; triggers are `push`(main) + `workflow_dispatch` only; report-only.

## 8. CI reference doc

- [x] 8.1 Write `docs/refs/ci.md` (English, per the docs language policy):
      triggers, the two workflows, the `ci/` script table, how to reproduce each
      step locally, how to read the report, and the macOS-now / Linux-later
      note. Link it from `docs/refs/README.md` and the `AGENTS.md` references
      list. DONE: `docs/refs/ci.md`; linked from `docs/refs/README.md` (+ the
      `.zh-cn.md` sibling, kept in sync) and the `AGENTS.md` references list.

## 9. Final verification

- [x] 9.1 Run the full local dry-run end to end: `build.sh dev-debug` +
      `ctest.sh dev-debug` (22/22) + `build.sh dev-release …` +
      `bench-verify.sh` (OK) + `size-report.sh` + `bench-report.sh`, each exit
      0, each emitting its report. DONE: 6/6 steps exit 0; reports emitted.
- [x] 9.2 `openspec validate add-github-ci --strict` passes. DONE: "Change
      'add-github-ci' is valid".
- [x] 9.3 Push the branch and confirm both workflows run green on GitHub
      Actions (the only step that needs the real runner); paste the run links.
      DONE via PR #1 (BillBai/gaby-vm). `ci.yml` green on the PR
      (run 27688475656, after the sticky-comment fix) and on push→main
      (run 27688478373); `bench.yml` green on its first push→main run
      (run 27688477840: native baseline built + ran, `bench.json` uploaded).
      The sticky PR comment renders the full report (22/22 tests, parity OK,
      size table).
