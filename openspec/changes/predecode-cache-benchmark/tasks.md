> Prerequisite: `predecode-cache-core` is implemented (the `gaby_vm`
> `PredecodeCache` / `Simulator` public API exists). This change consumes that
> API and is archived after it.

## 1. Cache engine in the shared runner

- [x] 1.1 Add `--engine {decoder|cache}` parsing to `bench/runner.{h,cc}`, defaulting to `decoder` (the existing behavior).
- [x] 1.2 Implement the cache engine: construct a `gaby_vm::PredecodeCache`, `RegisterCodeRange` over the workload's instruction buffer before the warm-up call, and drive `gaby_vm::Simulator::RunFrom`. Leave the decoder-engine path byte-for-byte unchanged.
- [x] 1.3 Keep the warm-up, steady-state timing, and LR-termination conventions identical across engines — confine all per-engine setup to outside the timed region so the measured loop is uniform.
- [x] 1.4 Emit an `engine` key in the key/value output identifying the engine that produced the run.

## 2. Build wiring

- [x] 2.1 Update `bench/CMakeLists.txt` so the runner consumes the `gaby_vm` public cache headers (`predecode_cache.h`, `simulator.h`); confirm `bench_baseline` and `bench_smoke` remain the only two benchmark binaries.
- [x] 2.2 Build under the `dev-release` preset with `GABY_VM_BUILD_BENCHMARKS=ON`; both binaries link and run.

## 3. Documentation

- [x] 3.1 Update `bench/README.md`: document the `--engine` flag, the new `engine` output key, and the cache-on vs cache-off comparison procedure.

## 4. Acceptance verification

- [x] 4.1 `bench_smoke --engine cache --seconds 0.2` and `bench_baseline --engine cache --seconds 1.0` exit cleanly and emit every spec-mandated output key plus `engine`.
- [x] 4.2 `--engine decoder` (and the no-flag default) reproduces the pre-change behavior; `ctest -N` still lists neither `bench_baseline` nor `bench_smoke`.
- [x] 4.3 On both workloads, the cache engine shows a meaningful, measured improvement over the decoder engine (`gaby-vm-predecode-cache-design.md` §4.5 acceptance #1 — soft; no hard `N×`).
- [x] 4.4 `git diff --name-only` for this change touches only paths under `bench/`; nothing under `src/`, `include/gaby_vm/`, or the imported VIXL tree.
- [x] 4.5 Run `openspec validate predecode-cache-benchmark --strict` (expect `valid`), then — once `predecode-cache-core` is archived — `openspec archive predecode-cache-benchmark` to apply the `benchmark-harness` deltas to the live spec.
