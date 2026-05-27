## Why

Sample profile `docs/refs/gaby-vm-cache-hotpath-profile-2026-05-27.md` shows
two remaining high-ROI items on the NEON cache hot path after the predecessor
change `neon-format-helpers-constexpr-inline` (archived 2026-05-27) landed:

1. `LogicVRegister::ClearForWrite` runs a byte-by-byte SetUint loop on every
   NEON write target — for half-width destinations this fires 8 dispatch calls
   + 8 `register_.Insert<uint8_t>` + 8 `NotifyRegisterWrite` flips when a
   single `memset` over the tail bytes plus one dirty-flag write would do.
   Profile attributes 12.1% of mixed cache runtime to ClearForWrite frame.
2. Seven VIXL `VectorFormat` helpers still live in `instructions-aarch64.cc`
   with external linkage. The same `constexpr inline` promotion that landed
   `neon-format-helpers-constexpr-inline` applies; they account for ~3-5% of
   mixed cache runtime spread across NEON leaves.

Stacking expected to bring mixed cache 21.97 → 18-19 ns/insn (~1.15-1.22×
over post-A, ~2.1-2.2× over original baseline), with a 5% bonus on mixed
decoder from B's effect on decoder NEON leaves.

## What Changes

- **Lever B**: rewrite `LogicVRegister::ClearForWrite` in
  `src/aarch64/simulator-aarch64.h` to call a new `SimRegisterBase::ClearTail`
  helper that does one `memset` over the tail bytes plus one
  `NotifyRegisterWrite()` call, replacing the per-byte SetUint loop. Behavior
  preserved: the only externally-visible difference is that the boolean
  `written_since_last_log_` dirty flag is set once per call instead of N
  times — both semantically equivalent.
- **Lever C**: promote the seven remaining imported VIXL `VectorFormat`
  helpers (`LaneSizeInBytesLog2FromFormat`, `MaxLaneCountFromFormat`,
  `ScalarFormatFromFormat`, `IsVectorFormat`, `MaxIntFromFormat`,
  `MinIntFromFormat`, `MaxUintFromFormat`) from `instructions-aarch64.cc` to
  `instructions-aarch64.h` with `constexpr inline` linkage, using the same
  marker-block convention and ordering discipline as the predecessor change.
- Extend `test/instructions_aarch64_constexpr_smoke_test.cc` with
  `static_assert` cases covering each newly-inlined helper.
- Record a new benchmark snapshot
  `docs/refs/baseline-benchmark-results-cache-2026-05-clearforwrite-helpers.md`
  with the post-change mixed/smoke cache and decoder numbers, plus a delta
  table against the post-A baseline.

## Capabilities

### New Capabilities
<!-- None — this change does not introduce a new capability. -->

### Modified Capabilities
- `aarch64-simulator`: adds normative requirements that ClearForWrite uses a
  bulk-clear path (no per-byte dispatch) and that the listed `VectorFormat`
  helpers are defined as `constexpr inline` to allow compile-time folding.

## Impact

- **Source files** (imported VIXL, marker-block edits only):
  - `src/aarch64/simulator-aarch64.h` (Lever B: new `ClearTail` helper +
    rewritten `ClearForWrite` body)
  - `src/aarch64/instructions-aarch64.h` (Lever C: 7 new `constexpr inline`
    helper definitions inside marker block)
  - `src/aarch64/instructions-aarch64.cc` (Lever C: 7 function bodies replaced
    by marker-block notes pointing back to the header)
- **Tests**:
  - `test/instructions_aarch64_constexpr_smoke_test.cc` (Lever C: extend
    `static_assert` coverage to the 7 new helpers; no new test target)
  - `workload_shadow` (Lever B language-level oracle: must report zero
    divergence)
- **Build**: no changes to `CMakeLists.txt` shape; no new compile defines.
- **Public API**: untouched. `PredecodedEntry` layout untouched.
- **Out of scope (deferred)**: Lever N (NEON temp buffer pool / kZRegMaxSize
  shrink) — re-profile after this change lands to pin down the real source
  of the residual 7.8% memset/memmove sample share, then open separately.
