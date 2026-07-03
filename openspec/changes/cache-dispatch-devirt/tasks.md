# Tasks: cache-dispatch-devirt

GRL = build dev-debug + dev-release → `ctest --test-dir build/debug -R
vixl_port` 3/3 → full debug ctest green → `bench_business --verify` OK →
3× `bench_business --mode cache --seconds 1.0` per shape, recorded in
numbers.md. Before-reference: T5 row (parse 8.455 / hash 7.177 / struct
9.384 / fsm 8.944 / applogic 10.416).

## 1. Ground truth

- [x] 1.1 Re-profile applogic at branch tip (profile build, `sample`, 12s)
      and record the dispatch-hub share in numbers.md. Abort/re-price the
      change if the hub is materially below ~20%.

## 2. Handler ABI + thunks

- [ ] 2.1 Internal handler machinery in a gaby marker block:
      `GabyHandler` typedef, handler-contract comment block (per design D1),
      `AsGabyHandler` cast helper + static_asserts.
- [ ] 2.2 Thunk-emitting macro over the visitor expansion lists; static
      member thunks reproducing the exact current per-step sequence
      (design D2/D3/D5); sentinel handlers with identical abort behavior.
- [ ] 2.3 `ResolvePredecodeHandler` + populate-pass switch to handler
      storage in `entry->leaf`; public-header comment wording update.
- [ ] 2.4 Shrink `ExecuteInstructionCached` to lookup + one handler call
      (design D5 audit contract).

## 3. Verification

- [ ] 3.1 Side-by-side audit: old hub sequence vs thunk body order (review
      gate, recorded as a checklist in the commit message or numbers.md).
- [ ] 3.2 GRL; record text-size delta of the release library/binary in
      numbers.md; acceptance = ≥ neutral bench + all gates green.

## 4. Wrap-up

- [ ] 4.1 numbers.md row + detail; supersession notes in
      `docs/refs/gaby-vm-predecode-cache-design.md` (D8 thunk deferral,
      "already fast enough" stance); commit(s) with task refs.
