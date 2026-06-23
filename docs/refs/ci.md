# Continuous Integration

gaby-vm runs CI on GitHub Actions. This is the reference for what runs, when,
and how to reproduce any of it locally. The normative contract lives in the
`continuous-integration` OpenSpec capability; this doc is the operational map.

## Design in one line

All CI logic is in **portable `ci/` shell scripts**; the workflow YAML only
orchestrates them. Every step a workflow runs, you can run locally on an arm64
mac with the same script and get the same result.

## Workflows

| workflow | triggers | runner | what it does | can it fail a PR? |
| --- | --- | --- | --- | --- |
| `.github/workflows/ci.yml` | pull request + push to `main` | `macos-14` | build (debug) + full CTest, build (release) + cache/decoder parity, iOS Simulator tests, binary-size report | yes — only on build error, test failure (host or iOS), or parity mismatch |
| `.github/workflows/bench.yml` | push to `main` + manual dispatch | `macos-14` | full benchmark harness + iOS Simulator benchmark → moving-baseline artifact | no (report-only) |

The split is deliberate: the **PR gate is deterministic**. GitHub's shared
runners are too noisy to gate on timing, so the only things that fail a PR are a
build break, a CTest failure (host or the iOS Simulator suites), or a
`bench_business --verify` parity mismatch (all bit-exact). Performance is measured on `main` as a moving baseline, never
as a PR gate.

## The `ci/` scripts

Each script is independently runnable; arguments are explicit.

| script | purpose | gates? |
| --- | --- | --- |
| `ci/util.sh` | sourced helpers (locate binaries, format sizes, append to the job summary, sticky PR comment) | — |
| `ci/build.sh <preset> [-D…]` | `cmake` configure + build | build error fails |
| `ci/ctest.sh <preset>` | full CTest suite (every test, including the `vixl_port` guard rail) | test failure fails |
| `ci/bench-verify.sh [preset]` | `bench_business --verify` — bit-exact cache==decoder parity | mismatch fails |
| `ci/size-report.sh [--base sizes.json]` | raw `libgaby_vm.a` + stripped `size_probe`; deltas vs base | never |
| `ci/bench-report.sh [--seconds s]` | whole harness: business three-way + mixed workload + smoke sanity | never |
| `ci/ios-test.sh` | build the iOS runner + run its XCTest correctness suites on an arm64 Simulator (skips `BenchTests`); skips loudly on a non-arm64 host | test failure fails |
| `ci/ios-bench.sh` | run `BenchTests` (business kernels, cache vs decoder) on an arm64 Simulator **in Release** (representative numbers); report-only | never |

`presets` are the CMake presets: `dev-debug` → `build/debug`, `dev-release` →
`build/release`.

## Reproduce CI locally

The full `ci.yml` dry-run on an arm64 mac:

```sh
ci/build.sh dev-debug
ci/ctest.sh dev-debug                                   # 22/22 expected
ci/build.sh dev-release -DGABY_VM_BUILD_BENCHMARKS=ON
ci/bench-verify.sh dev-release                          # "verify: OK"
ci/size-report.sh --out sizes.json                      # raw + stripped sizes
ci/ios-test.sh                                          # iOS Simulator suites (arm64 host)
```

The iOS scripts generate the runner with `ios-runner/generate.sh` (installing
XcodeGen on demand) and need an arm64 iOS Simulator; on any other host they skip
loudly and exit 0. See [`docs/ios.md`](../ios.md) for the runner itself.

The `bench.yml` run (adds the native baseline, arm64-only):

```sh
ci/build.sh dev-release -DGABY_VM_BUILD_BENCHMARKS=ON -DGABY_VM_BUILD_NATIVE_BASELINE=ON
ci/bench-report.sh --out bench.json                     # three-way table + JSON
ci/ios-bench.sh                                         # iOS cache/decoder (report-only)
```

Without `$GITHUB_STEP_SUMMARY` set, the scripts print their markdown report to
stdout, so a local run shows exactly what CI would publish.

## Binary size

Two numbers, both **report-only** (a size increase never fails a PR):

- **raw** — `libgaby_vm.a` file size. Trivial, overcounts (unstripped, includes
  unused objects), catches gross regressions.
- **stripped `size_probe`** — a minimal executable (`bench/size_probe.cc`) that
  links only the public `Simulator` API, stripped. This is the honest "what an
  embedder ships" footprint; it is large because exercising the simulator pulls
  in the whole VIXL leaf-dispatch surface. The PR delta is the number that
  matters — it isolates the change a PR makes to the public footprint.

PR deltas come from the latest successful `main` run's `sizes.json` artifact
(fetched with `gh run download`). On the first run, or if the artifact has
expired, the report shows absolute sizes and notes that no base is available.

## Reading the report

- **Job summary** — every run writes a markdown report (test result, parity
  result, size table) to the Actions run page.
- **Sticky PR comment** — on pull requests, the same report is posted as a
  single comment, updated in place on re-runs (`gh pr comment --edit-last`), so
  the thread isn't spammed.

## Platforms

macOS arm64 is the only runner today: it matches the primary iOS target's ISA
and is the only place the native baseline (the slowdown-vs-native denominator)
is meaningful — GitHub's standard Linux runners are x86_64. A Linux
`build + ctest` job (a portability guard for the Linux/Android/HarmonyOS
targets) is a planned follow-up; the `ci/` script seam is what makes adding it
cheap.
