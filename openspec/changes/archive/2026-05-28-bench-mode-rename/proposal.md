## Why

The two execution paths through the benchmark harness — one driving the
imported `vixl::aarch64::Simulator` directly, the other driving
`gaby_vm::Simulator` over a `PredecodeCache` — are referred to
inconsistently across the codebase. The code uses **engine** (the
`enum class Engine`, the `--engine` CLI flag, the `engine:` output key,
the `RunDecoderEngine` / `RunCacheEngine` functions, `bench/README.md`,
and the live `benchmark-harness` spec). Architecture and design docs
drift between "engine", "mode", "cache-on / cache-off", "cached
dispatch path", and "predecode/dispatch cache". A reader trying to map
a doc paragraph to a CLI flag or a code location has to translate
between several near-synonyms.

This change picks a single noun — **mode** — and a single pair of
labels — **decoder** and **cache** — and applies it everywhere: code,
CLI, output keys, README, architecture / design docs, and the
`benchmark-harness` spec. It also writes the convention down in
`docs/conventions.md` so future docs do not drift again.

The labels stay in English (`decoder` / `cache`) so the same identifier
serves the CLI flag value, the output key, English prose, and Chinese
prose alike. A `git grep decoder` or `git grep cache` returns hits
regardless of which document layer or human language a reader is
searching, which is the whole point of standardising.

## What Changes

This is a one-shot rename. No execution-path semantic changes. No new
files outside the openspec change directory and the new "Terminology"
section in `docs/conventions.md`.

### Code

- `bench/runner.cc`:
  - `enum class Engine { kDecoder, kCache }` → `enum class Mode { kDecoder, kCache }`.
  - `EngineName(Engine)` → `ModeName(Mode)`.
  - `RunDecoderEngine(...)` → `RunDecoderMode(...)`; `RunCacheEngine(...)` → `RunCacheMode(...)`.
  - `Args::engine` (default `Engine::kDecoder`) → `Args::mode` (default `Mode::kDecoder`).
  - CLI flag `--engine {decoder|cache}` → `--mode {decoder|cache}`. The flag *values* are unchanged; only the flag *name* moves.
  - Usage block, error messages ("missing value for --engine", "invalid --engine value: ...", "cache engine: RegisterCodeRange failed: ...") rewrite "engine" → "mode".
  - Output line `engine: <value>` → `mode: <value>`.
  - File-level and function-level comments that say "engine" rewrite to "mode".
- `bench/CMakeLists.txt`: the two comment lines that mention "decoder engine" / "cache engine" rewrite to "decoder mode" / "cache mode".
- `bench/runner.h`, `bench/baseline.cc`, `bench/smoke.cc`: scan for residual references during implementation; update if found.

### CLI flag — no compatibility alias

`--engine` is **not** kept as a deprecated alias for `--mode`. The
bench harness is a developer-only tool with no committed external
consumers, and a clean rename costs less than carrying a permanent
two-name surface. This matches the project's stance during
`predecode-cache-types-rename` (V1 is pre-stability; cosmetic surface
breakage is acceptable). Anyone with a local script invoking
`--engine cache` updates to `--mode cache`.

### Documentation (non-normative)

- `bench/README.md`: every reference to "engine", "the engine", "cache
  engine", "decoder engine", "cache-on / cache-off", "engines"
  rewrites to the corresponding "mode" form. The examples block
  updates the flag spelling. The output-keys table renames the
  `engine` row to `mode`.
- `docs/refs/gaby-vm-predecode-cache-design.md`: the table column
  header `engine` and the CLI invocation examples
  (`bench/bench_baseline --engine ...`) follow the rename. The "两组
  engine" Chinese prose is rewritten.
- `docs/refs/baseline-benchmark-suite.md`: three references to "both
  engines" rewrite to "both modes".
- `docs/architecture.md`: phrases that drift around the same concept —
  "cached dispatch path", "predecode/dispatch cache" *when used as a
  mode name* — rewrite to "cache mode". The data structure name
  `PredecodeCache` itself is preserved; mechanism-level phrasing
  ("predecode once → cache decoded dispatch target → execute cached
  path repeatedly") is preserved.
- `docs/conventions.md`: a new "Terminology" section pins the names
  (`decoder mode` / `cache mode`), states that identifiers stay in
  English even in Chinese prose (so the Chinese rendering is "decoder
  模式" / "cache 模式", not "解码器模式" / "缓存模式"), and lists
  forbidden flutter phrasings.

### Documentation explicitly **not** edited

- `docs/refs/baseline-benchmark-results-*.md` — historical benchmark
  result snapshots that record what the CLI flag actually printed at
  the time of the run. Editing them would falsify the historical
  record.
- `docs/archive/` — archived working notes; out of scope.
- `AGENTS.md` (alias `CLAUDE.md`) clause "execute cached path
  repeatedly" — describes the *mechanism*, not a mode name.
- `docs/refs/vixl-overview.md` reference to "execution engine" —
  refers to upstream VIXL's terminology.
- Standard-library identifiers like `std::linear_congruential_engine`.

### Spec

`openspec/specs/benchmark-harness/spec.md` currently contains the
requirement **"Benchmark harness supports a cache execution engine"**
plus four scenarios that name "engine". The Requirement's title
itself contains "engine", so the delta in this change expresses the
rewrite as a `REMOVED Requirements` entry plus an `ADDED Requirements`
entry. The body content (selectable at invocation, pre-timing
registration, output identification, single-binary constraint) is
preserved — only the user-visible vocabulary moves.

## Capabilities

### New Capabilities

*(none — this is a rename inside an existing capability.)*

### Modified Capabilities

- `benchmark-harness`: the requirement currently titled "Benchmark
  harness supports a cache execution engine" is removed and re-added
  as "Benchmark harness supports a cache execution mode". All
  behavioural content — selectability at invocation, pre-timing
  registration, output identification, single-binary constraint — is
  preserved verbatim modulo the engine→mode word swap; only the
  user-visible vocabulary moves.

## Impact

- **Breaking surface**: `--engine` CLI flag, `engine:` output key, the
  `Engine` enum and `Run*Engine` functions inside `bench/runner.cc`.
  All three are inside `bench/`; nothing under `src/`,
  `include/gaby_vm/`, or the imported VIXL tree changes.
- **No execution-path change**: decoder mode keeps driving
  `vixl::aarch64::Simulator`; cache mode keeps driving
  `gaby_vm::Simulator` over `PredecodeCache`. Warm-up and timed-loop
  shapes are byte-for-byte unchanged.
- **CMake**: unaffected (no new targets, no new options, no
  build-flag changes).
- **CTest**: unaffected — neither `bench_baseline` nor `bench_smoke` is
  registered with CTest.
- **External scripts**: any local script invoking `--engine` updates
  to `--mode`. There are no committed external consumers in this
  repository.
- **Historical benchmark snapshots** are kept as-is, by design.
- **Out of scope for this change**:
  - Any execution-path or simulator-leaf semantic edit.
  - Renaming the data structure `PredecodeCache` itself.
  - Touching imported VIXL files.
  - Introducing a `--engine` deprecation alias.
