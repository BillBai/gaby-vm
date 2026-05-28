> Prerequisite: none. This change is self-contained — code, docs, and
> the `benchmark-harness` spec delta are all in this proposal.

## 1. Convention docs

- [ ] 1.1 Add a "Terminology" section to `docs/conventions.md` that defines `decoder mode` and `cache mode`, states the identifiers-stay-English policy (so Chinese prose says "decoder 模式" / "cache 模式", not "解码器模式" / "缓存模式"), and lists forbidden flutter phrasings (`cache-on / cache-off`, `cache engine`, `decoder engine`, `cached dispatch path` when used as a mode name).

## 2. Code rename

- [ ] 2.1 `bench/runner.cc`: rename `enum class Engine` → `Mode`, `EngineName` → `ModeName`, `RunDecoderEngine`/`RunCacheEngine` → `RunDecoderMode`/`RunCacheMode`, `Args::engine` → `Args::mode`.
- [ ] 2.2 `bench/runner.cc`: rename CLI flag `--engine` → `--mode` (parsing, usage block, error messages).
- [ ] 2.3 `bench/runner.cc`: rename output key `engine: <value>` → `mode: <value>`.
- [ ] 2.4 `bench/runner.cc`: rewrite file-level and function-level comments ("Cache engine —", "Decoder engine —", "Which execution engine drives the workload.", "cache engine: RegisterCodeRange failed: …", etc.) to use "mode".
- [ ] 2.5 `bench/CMakeLists.txt`: rewrite the two comment lines that mention "decoder engine" / "cache engine" to "decoder mode" / "cache mode".
- [ ] 2.6 Scan `bench/runner.h`, `bench/baseline.cc`, `bench/smoke.cc` for residual "engine" references; update any found.

## 3. Documentation rewrites

- [ ] 3.1 `bench/README.md`: rewrite all "engine" prose to "mode"; update the `--engine` CLI examples to `--mode`; rename the output-keys table row `engine` → `mode`; replace "cache-on / cache-off" with "cache mode / decoder mode".
- [ ] 3.2 `docs/refs/gaby-vm-predecode-cache-design.md`: rename the table column header `engine` → `mode`; update the two CLI invocation examples (`--engine <decoder|cache>` → `--mode <decoder|cache>`); rewrite "两组 engine" Chinese prose.
- [ ] 3.3 `docs/refs/baseline-benchmark-suite.md`: rewrite the three "both engines" mentions to "both modes".
- [ ] 3.4 `docs/architecture.md`: rewrite "cached dispatch path" / "predecode/dispatch cache" prose around the *mode* concept to use "cache mode". Preserve the data-structure name `PredecodeCache` and mechanism-level phrasing ("execute cached path repeatedly", etc.).

## 4. Spec delta application

- [ ] 4.1 The `specs/benchmark-harness/spec.md` delta in this change captures the live-spec rewrite. No additional spec edits during implementation.
- [ ] 4.2 `openspec validate bench-mode-rename --strict` reports `valid` after authoring (also at the end of implementation, in case any task updates the proposal).

## 5. Acceptance verification

- [ ] 5.1 Build: `cmake --build --preset dev-release` (or equivalent) — `bench_baseline` and `bench_smoke` link clean after the rename.
- [ ] 5.2 Smoke: `bench_smoke --mode cache --seconds 0.2` and `bench_baseline --mode cache --seconds 1.0` exit 0 and emit `mode: cache` in their key/value output.
- [ ] 5.3 Smoke: `bench_smoke --mode decoder --seconds 0.2` and `bench_baseline` (no flag) emit `mode: decoder` and reproduce the pre-rename behaviour.
- [ ] 5.4 Negative: `bench_smoke --engine cache` fails with an "unknown argument" error pointing at `--help`, exit status 2.
- [ ] 5.5 Search: `git grep -nE '(--engine|EngineName|RunDecoderEngine|RunCacheEngine|enum class Engine|^engine:)'` returns no hits inside `bench/`, `docs/conventions.md`, `docs/architecture.md`, or `openspec/specs/benchmark-harness/spec.md`. Allowed residuals: `docs/refs/baseline-benchmark-results-*.md`, `docs/archive/*`, `docs/refs/vixl-overview.md`, `std::*_engine` standard-library names.
- [ ] 5.6 `git diff --name-only` for this change touches paths under `bench/`, `docs/`, and `openspec/`. Nothing under `src/`, `include/gaby_vm/`, or the imported VIXL tree is modified.
- [ ] 5.7 `openspec archive bench-mode-rename` applies the `benchmark-harness` delta to the live spec, leaving the live spec textually consistent with the renamed code/docs.
