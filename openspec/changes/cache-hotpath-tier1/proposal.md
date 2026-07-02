# Proposal: cache-hotpath-tier1

## Why

The 2026-07-02 exploration round (nine-agent readings + adversarial review, building on
[`docs/refs/gaby-vm-dispatch-flatten-profile-2026-06-11.md`](../../../docs/refs/gaby-vm-dispatch-flatten-profile-2026-06-11.md)
and a fresh `applogic` cache-track profile) identified six small, independent,
measurement-backed hot-path optimizations that recover ~10–15% on the scalar
business kernels and ~25–30% on `applogic` without touching the frozen entry
layout, `form_hash_`, or any leaf semantics. They are the highest
impact-per-effort tier of the sanctioned optimization direction (predecode
once → cached dispatch → repeated execution) and are prerequisites for the
later flatten/specialize milestones.

## What Changes

- **T1 — LogicVRegister right-sizing**: shrink the SVE-max-provisioned
  `saturated_[1024]` / `round_[256]` scratch arrays to the 16 lanes reachable
  at the pinned VL=128, bound `SimRegisterBase::Write`'s clear to the actual
  register size, and hoist the per-call `unordered_map` in
  `SimulateFPRoundInt[ToSize]` to static storage. Measured target: the ~15%
  memset/memmove/chkstk bucket in the `applogic` profile.
- **T2 — Load/store leaf de-layering**: gate the dead trace-preparation tail
  in `LoadStoreHelper`/`LoadStorePairHelper` on the trace mask; early-return
  `LocalMonitor::MaybeClear`'s LCG when the monitor is unarmed (separate
  commit — deviates from upstream's PRNG advance sequence); precompute
  `SimStack` guard-region bounds.
- **T3 — MOVPRFX flag-gating**: classify MOVPRFX forms at predecode time into
  a `PredecodedEntry::flags` bit; replace the per-step `form_hash_`
  compare-pair in `ExecuteInstructionCached` with the predecoded bit. The
  `CanTakeSVEMovprfx` violation check still fires. Prerequisite: hand-encoded
  positive/negative MOVPRFX protocol tests (there is currently **zero**
  executed coverage of this protocol — vixl_port has no SVE bodies).
- **T4 — Dispatch-hub epilogue strip**: single trace-mask test instead of
  three in `LogAllWrittenRegisters`, conditional `UpdateBType`, fold the
  `kEndOfSimAddress` global load to a null compare on the cache track, move
  the abort-path `ostringstream` out of `ExecuteInstructionCached`, add
  likely/unlikely hints.
- **T5 — Branch-interception probe flag**: skip the per-BR/BLR
  `unordered_map::find` when no interception was ever registered (flag on
  `MetaDataDepot`, kept in sync by registration/reset).
- **T6 — AddWithCarry flag-skip**: when `set_flags` is false, compute only the
  masked sum. Gated on first inspecting release disassembly — clang may
  already sink the NZCV computation.

Every item: `ctest -R vixl_port` green before/after, `bench_business
--verify`, per-shape before/after numbers from `bench_business` (M1 Pro
working baseline: parse 9.34 / hash 10.49 / struct 10.81 / fsm 9.34 /
applogic 14.26 ns/insn, 2026-07-03).

## Capabilities

### New Capabilities

(none)

### Modified Capabilities

- `predecode-cache`: add a scenario pinning that per-step MOVPRFX
  classification is predecode-derived (a `flags` bit) while the
  `CanTakeSVEMovprfx` violation check still aborts — this converts an
  untested runtime protocol into a spec-mandated, tested one. Entry layout
  scenario is unchanged (16 bytes, same fields).

## Impact

- Imported files (marker convention required): `simulator-aarch64.h`,
  `simulator-aarch64.cc`, `logic-aarch64.cc` (T1/T2/T4/T5/T6).
- gaby-vm-owned: `predecode_cache.cc` predecode classification (T3),
  cache-track region of `ExecuteInstructionCached` (T3/T4).
- Tests: new MOVPRFX protocol unit tests; new VL-bound guard asserts (T1).
  `docs/refs/vixl-extraction-map.md` gets a note that widening SVE coverage
  must revisit the T1 lane bound.
- No public API, entry layout, `form_hash_`, or benchmark-harness changes.

## Out of scope

- Tier 2/3 of the exploration (flat-thunk devirtualization, range-local
  hoisting, unity TU/LTO, specialized handlers, PGO) — follow-up changes,
  re-estimated after this tier's re-profile.
- Threaded dispatch (direct threading + `[[clang::musttail]]` tail-call
  chaining, requested 2026-07-03): planned as the natural extension of the
  flat-thunk follow-up — the thunk table *is* the direct-threading data
  structure; the chaining experiment needs thunks to exist first and this
  tier's T3/T4 to thin the per-step bookkeeping that would have to move into
  the thunks. No RWX/JIT implications (handler addresses are ordinary data).
- CMP+B.cond fusion (conflicts with the spec's mix-independence clause) and
  pmf-table densification (mooted by the thunk work).
- Any `form_hash_` removal or entry-layout change.
