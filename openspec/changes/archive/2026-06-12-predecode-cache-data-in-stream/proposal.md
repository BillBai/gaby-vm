## Why

Real AArch64 code interleaves read-only **data** with instructions in one
executable range — literal pools (`LDR x, =imm`), jump tables, inline
constants. `PredecodeCache::RegisterCodeRange` predecodes every 4-byte word as
an instruction, and when a data word decodes to an unallocated encoding (common
for arbitrary 64-bit constants split into two words) it **rejects the whole
range**, so the surrounding real code can never use the cache track. This is the
exact input shape the predecode/dispatch optimization exists to accelerate, so
the limitation sits on the critical path of the project's main goal. In the
`vixl_port` guard rail it shows up as 5 skipped cases (4 `ldr_literal*`,
`fjcvtzs`). Full analysis:
[ref doc](../../../docs/refs/gaby-vm-predecode-cache-data-in-stream-2026-06-10.md).

## What Changes

- A word that does not decode to an executable cached instruction — an
  **unallocated** encoding, or (forward-looking) one the auditor would reject —
  is recorded as a **non-executable data sentinel** and the range registers
  `Ok`, instead of failing the range. Same trap-on-execute model already used
  for unimplemented forms (design.md R12): the sentinel reuses the imported
  `Simulator::VisitUnallocated`, so a wild PC that reaches a data word still
  aborts with its address — no edit to imported code.
- **BREAKING (contract):** `RegisterCodeRange` no longer returns
  `UnsupportedFeature`. The cache auditor is hardwired to `CPUFeatures::All()`,
  which disables feature checking, so this status was already unreachable in
  practice; the enum value is retained for ABI stability but is no longer
  produced. Per-word decode properties (unallocated, feature-gated) never reject
  a range; only structural problems do (bad size, overlap, OOM).
- Reconcile the reversed design decision: update design.md R12 and the
  all-or-nothing / `UnsupportedFeature` rationale (`...design.md:299`).
- Rebaseline the `vixl_port` coverage counts (`kFamilyBaselines`) for the
  revived cases (measured, not assumed).

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `predecode-cache`: relax the "validates inputs and reports failures
  all-or-nothing" requirement (drop `UnsupportedFeature` as a returned status);
  add a requirement that a range with embedded read-only data registers `Ok`,
  its data words are non-executable sentinels, loads of the data read the
  original bytes, and a PC that reaches a data word traps.

## Impact

- Code: ~15 lines in `Sources/gaby_vm/src/gaby_vm/predecode_cache.cc` (a new
  sentinel helper + collapsing the reject branch); no public-header change.
- Tests: new focused cache unit test (`test/predecode_cache_data_in_stream_test.cc`,
  registered in `test/CMakeLists.txt`); rebaseline `kFamilyBaselines` in the
  `vixl_port` harness — the 5 skipped cases revive and run on both tracks.
- Docs: `docs/refs/gaby-vm-predecode-cache-design.md` (R12 + §4.3.2 rationale).
- Guard rail: `ctest -R vixl_port` green under `dev-debug` and `dev-release`
  before and after.
