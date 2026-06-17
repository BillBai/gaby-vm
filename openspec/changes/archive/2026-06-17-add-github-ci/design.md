# Design

## Key decisions

### 1. CI logic lives in `ci/` scripts; YAML only orchestrates

Every build/test/bench/size step is a shell script under `ci/`. The workflow
files do checkout + runner setup + invoke a script + publish the result. This
is the one structure that makes "add a Linux runner later" nearly free (the
same scripts run on both) and lets any CI result be reproduced locally on an
arm64 mac — which is the whole point, since this repo's perf and parity work is
done by hand today. The trade-off (a layer of shell indirection over inline
`run:` steps) is worth it for local reproducibility and the future matrix.

Script contract (each is independently runnable; arguments are explicit, no
hidden globals):

| script | purpose | gates? |
|---|---|---|
| `ci/util.sh` | sourced helpers: locate repo root + built binaries, format sizes, emit a markdown table, append to `$GITHUB_STEP_SUMMARY`, write JSON | n/a |
| `ci/build.sh <preset> [-D…]` | `cmake` configure + build wrapper | build error fails |
| `ci/ctest.sh <preset>` | full CTest suite (all tests, not just the guard rail) | test failure fails |
| `ci/bench-verify.sh <preset>` | `bench_business --verify` parity, all kernels | mismatch fails |
| `ci/size-report.sh [--base sizes.json]` | raw `.a` + stripped `size_probe`; deltas vs base; emit `sizes.json` + markdown | never |
| `ci/bench-report.sh` | full harness: business three-way + mixed baseline + smoke sanity; emit `bench.json` + markdown | never |

"Utilize all we have": `ctest.sh` runs the entire suite (the 22 tests including
the `vixl_port` guard rail); `bench-report.sh` drives `bench_business`,
`bench_baseline`, `native_business`, `native_baseline`, and `bench_smoke` — not
just one binary.

### 2. The PR gate is deterministic; perf is never a PR gate

GitHub's shared runners are noisy, so timing numbers can't gate a PR without
flaking. The split:

- **Pull request (`ci.yml`)** runs only deterministic checks — build, full
  CTest, and `bench_business --verify` (a bit-exact cache==decoder cross-check,
  zero timing). A PR fails on exactly: build error, CTest failure, or parity
  mismatch.
- **Push to `main` + manual dispatch (`bench.yml`)** runs the timing harness
  and records a **moving baseline** artifact. Report-only.

This keeps PRs fast and trustworthy while still tracking perf over time.

### 3. Build matrix per job

Tests want debug (VIXL `VIXL_ASSERT`s live — they catch cache bugs the guard
rail exists for); size and the bench binaries want release. So `ci.yml`'s one
job builds **debug** (→ `ctest.sh`) then **release** with benchmarks on (→
`bench-verify.sh`, `size-report.sh`). `bench.yml` builds **release + native
baseline** (arm64-only) for `bench-report.sh`. Two builds in `ci.yml` is the
honest cost of running asserts *and* measuring the shipped artifact; both are
cached by the runner's compiler cache across steps.

### 4. Binary size: raw + stripped, delta vs base, report-only

Two numbers, per the approved design:

- **raw** — `stat` of `libgaby_vm.a` (release). Trivial, overcounts (unstripped,
  includes unused objects), but catches gross regressions.
- **stripped embedder footprint** — link a minimal **`size_probe`** unit that
  touches the public `Simulator` API, with linker dead-strip + `strip`, then
  measure `__TEXT`+`__DATA` via `size`. This is the honest "what an iOS app
  ships" figure; a non-minimal binary like `bench_business` would inflate it
  with harness + kernel data.

`size_probe` is a tiny new target behind the existing `GABY_VM_BUILD_BENCHMARKS`
option (no new option, no change to shipping `Sources/gaby_vm/src/`).

**Delta mechanism (PR vs base):** on push to `main`, `ci.yml` uploads
`sizes.json` as an artifact. On a PR, the job fetches the latest successful
`main` run's `sizes.json` via `gh run download` and passes it to
`size-report.sh --base`. If none exists yet (first run), the report shows
absolute sizes with "no base to compare" and still passes. This avoids a second
base-branch build in every PR; a missing base is harmless because size is
report-only.

### 5. Reports: job summary + sticky PR comment

Every run writes a markdown report (test result, parity result, size table) to
`$GITHUB_STEP_SUMMARY`. On pull requests, the same report is posted as a
**single sticky comment** keyed by a hidden marker, updated in place on
re-runs so the thread isn't spammed. Uses the default `GITHUB_TOKEN` with
`pull-requests: write`. `bench.yml` appends its three-way table to the job
summary and uploads `bench.json`.

### 6. Runner: macOS arm64 now, Linux later

`macos-14` is Apple Silicon — the only GitHub runner where the native baseline
and slowdown-vs-native figures mean anything, and it matches the primary iOS
target's ISA. GitHub's standard Linux runners are x86_64 and can't produce the
native denominator. A Linux `build + ctest` job (portability guard for the
Linux/Android/HarmonyOS targets) is a deliberate later follow-up; the `ci/`
script seam is what makes adding it cheap.

## Workflow shapes (non-obvious bits only)

```yaml
# ci.yml — concurrency cancels superseded PR runs; least-privilege token
concurrency: { group: ci-${{ github.ref }}, cancel-in-progress: true }
permissions: { contents: read, pull-requests: write }
on: { pull_request: {}, push: { branches: [main] } }
```

```yaml
# bench.yml — main + manual only, no PR trigger, no special permissions
on: { push: { branches: [main] }, workflow_dispatch: {} }
```

The sticky comment is "find a comment whose body contains
`<!-- gaby-ci-report -->`, update it if found, else create it" — implemented in
`ci/util.sh` over the `gh` CLI (preinstalled on runners), not a third-party
action, to keep the dependency surface at zero.

## Testing approach

CI scripts are shell, so "tests" means running each script locally against a
real build on this arm64 host and confirming its output (exit code, emitted
markdown, `sizes.json`/`bench.json` shape) — see `tasks.md`. The end-to-end
GitHub run is verified once the branch is pushed and Actions goes green; until
then every script is validated locally, which approach A makes possible.
