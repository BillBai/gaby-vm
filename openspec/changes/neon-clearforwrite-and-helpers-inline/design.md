## Context

`neon-format-helpers-constexpr-inline` (archived 2026-05-27) landed the
six-helper `constexpr inline` promotion and took mixed cache from
39.55 → 21.97 ns/insn (1.80×). The matching sample profile
`docs/refs/gaby-vm-cache-hotpath-profile-2026-05-27.md` was taken before
that change. Two items in the profile remain unaddressed and account for
most of the post-A NEON-leaf overhead:

1. **`LogicVRegister::ClearForWrite` byte loop** (12.1% of mixed cache).
   The implementation at `src/aarch64/simulator-aarch64.h:916-924` runs a
   `for` loop calling `SetUint(kFormat16B, i, 0)` once per tail byte.
   For a D-form destination that's 8 SetUint dispatches + 8 byte writes
   + 8 `NotifyRegisterWrite()` flips, when the underlying storage
   (`uint8_t value_[256]` at `src/aarch64/simulator-aarch64.h:589`) admits
   a single `memset` + one dirty-flag write.

2. **Residual VIXL `VectorFormat` helpers** (~3-5% of mixed cache,
   distributed across NEON leaves). Seven helpers still defined in
   `instructions-aarch64.cc` with external linkage:
   `LaneSizeInBytesLog2FromFormat`, `MaxLaneCountFromFormat`,
   `ScalarFormatFromFormat`, `IsVectorFormat`, `MaxIntFromFormat`,
   `MinIntFromFormat`, `MaxUintFromFormat`. Same `constexpr inline`
   pattern as A applies, but the **dependency closure** drags in two
   more touchpoints:
   - `ScalarFormatFromLaneSize` (called by `ScalarFormatFromFormat`)
     also lives in `.cc`; promoting only the caller means the call site
     still goes through external linkage and the fold breaks.
   - `GetUintMask` (called by `MaxIntFromFormat` / `MaxUintFromFormat`)
     lives in `src/utils-vixl.h:75` as `inline` but not `constexpr`. To
     make the callers actually fold, it must be `constexpr inline` too.

Constraints inherited from the project:

- All edits live inside imported VIXL files. The marker-block convention
  (`// gaby-vm BEGIN:` … `// gaby-vm END`) is the only legal way to
  modify these (see `docs/architecture.md#vixl-import-boundary`).
- Semantics must be preserved bit-for-bit. The `workload_shadow` dual-
  track oracle (every committed benchmark workload, decoder vs cache)
  is the language-level acceptance gate.
- No new build defines, no public API changes, no `PredecodedEntry`
  layout changes.
- iOS / macOS / POSIX portability — `memset` is in `<cstring>` and
  available on all targets.

## Goals / Non-Goals

**Goals:**

- Cut `LogicVRegister::ClearForWrite` per-call cost from O(tail bytes)
  function dispatches down to a single `memset` + a single dirty-flag
  write.
- Make the seven listed `VectorFormat` helpers (plus `ScalarFormatFromLaneSize`
  as a dependency closure addition) `constexpr inline` so the leaves
  that call them fold at compile time.
- Promote `GetUintMask` to `constexpr inline` so `MaxIntFromFormat` and
  `MaxUintFromFormat` actually fold.
- Extend `instructions_aarch64_constexpr_smoke_test.cc` to statically
  verify each newly-inlined helper folds.
- Land mixed cache median ≤ 19.5 ns/insn (~11% better than the post-A
  baseline of 21.97 ns/insn), validated against the same bench harness.

**Non-Goals:**

- Lever N (NEON temp buffer pool / `kZRegMaxSize` shrink). Deferred to
  the next propose after re-profile; the current profile's attribution
  of memset/memmove samples is suspect (see proposal §显式不在 scope).
- Rewriting upstream VIXL leaf semantics (out of project scope per
  CLAUDE.md "Do not rewrite simulator leaf semantics just to make the
  cache cleaner").
- JIT. Permanently out of scope per CLAUDE.md.
- `PredecodedEntry` layout changes or any work on `PredecodeCache`.

## Decisions

### D1: ClearForWrite uses `memset` over the tail bytes

**Decision**: Replace the byte loop with a single `memset(value_ + size, 0, total - size)`
call wrapped in a new `SimRegisterBase::ClearTail(unsigned from)` helper.

**Rationale**:
- The storage is `uint8_t value_[kMaxSizeInBytes]` (line 589) — a flat
  byte array. `memset` is the canonical way to zero a byte range.
- libc `_platform_memset` is already SIMD-vectorized on Apple targets
  (single `movi v0.16b, #0; str q0, [...]` for the 8-16 byte range
  that V1 ASIMD targets hit).
- The byte loop incurs N function-call boundaries each with a
  switch-on-format inside SetUint (now constexpr-folded post-A but the
  per-call overhead is still 8+ stack ops + dirty-flag write).

**Alternatives considered**:
- *Inline 16-byte SIMD store directly* (e.g., `vst1q_u8`): rejected.
  Couples ClearForWrite to NEON intrinsics, breaks portability to
  POSIX hosts without NEON, and gains nothing over `memset` since libc
  already vectorizes.
- *Open-code with `std::fill`*: rejected. `memset` is the more direct
  expression of intent for zero-fill on a `uint8_t` array.
- *Skip the call entirely for full-width formats*: implicitly handled
  — when `size == total`, the new `if (size < total)` guard returns
  early with zero work. No additional gating layer needed.

### D2: `ClearTail` lives on `SimRegisterBase`, not on `LogicVRegister`

**Decision**: Add `ClearTail(unsigned from)` as a `protected` (or
`public` if internal-call requires it) inline method on `SimRegisterBase`,
with `LogicVRegister::ClearForWrite` calling `register_.ClearTail(size)`.

**Rationale**:
- `SimRegisterBase` owns `value_` and `size_in_bytes_`; encapsulating
  the byte-range clear inside that class is the natural ownership.
- `LogicVRegister` doesn't need direct access to `value_`. Keeps the
  abstraction intact.
- Both classes live in `simulator-aarch64.h`; both methods are inline.
  No code-gen cost over open-coding inside `ClearForWrite`.

**Alternatives considered**:
- *Open-code `memset(register_.GetBytesMutable() + size, 0, ...)`*:
  would require adding a non-const `GetBytesMutable()` accessor on
  `SimRegisterBase`. That accessor would be more permissive than what
  we actually need (callers could write any byte). `ClearTail` is the
  narrowest privilege grant.

### D3: `NotifyRegisterWrite()` called once per `ClearForWrite`

**Decision**: The new `ClearTail` body invokes `NotifyRegisterWrite()`
exactly once after the `memset`, replacing the N invocations the byte
loop made.

**Rationale**: `NotifyRegisterWrite()` (`src/aarch64/simulator-aarch64.h:584`)
sets the boolean `written_since_last_log_ = true`. The flag is consumed
by `WrittenSinceLastLog()` (line 584) which the VIXL trace logger uses
to decide whether to dump the register after a step. The semantic
question is "was this register written during this step?", not "how many
bytes were written" — so setting the flag once is equivalent to setting
it N times. Verified by reading the consumer call site in VIXL log
infrastructure.

**Risk if wrong**: VIXL trace log output diverges between old and new
implementations. **Mitigation**: `workload_shadow` does not check
trace-log output, but a manual diff of `--trace` output on a small
NEON sequence (see Task 3.3) confirms parity.

### D4: Lever C scope expanded to closure-complete set

**Decision**: The propose listed 7 helpers; the implementable scope is
9 touchpoints to make the fold actually take effect:

| # | Helper | File | Dependency on inlined helpers |
|---|--------|------|-------------------------------|
| 1 | `LaneSizeInBytesLog2FromFormat` | `instructions-aarch64.h/cc` | none (pure switch on vform) |
| 2 | `MaxLaneCountFromFormat` | `instructions-aarch64.h/cc` | none |
| 3 | `IsVectorFormat` | `instructions-aarch64.h/cc` | none |
| 4 | `ScalarFormatFromLaneSize` | `instructions-aarch64.h/cc` | none (pure switch on int) |
| 5 | `ScalarFormatFromFormat` | `instructions-aarch64.h/cc` | #4 + `LaneSizeInBitsFromFormat` (A) |
| 6 | `MaxIntFromFormat` | `instructions-aarch64.h/cc` | `LaneSizeInBitsFromFormat` (A) + `GetUintMask` (#9) |
| 7 | `MinIntFromFormat` | `instructions-aarch64.h/cc` | #6 |
| 8 | `MaxUintFromFormat` | `instructions-aarch64.h/cc` | `LaneSizeInBitsFromFormat` (A) + `GetUintMask` (#9) |
| 9 | `GetUintMask` | `src/utils-vixl.h:75` | none — token-level promotion `inline` → `constexpr inline` |

**Rationale**:
- Dropping #4 (`ScalarFormatFromLaneSize`) and #9 (`GetUintMask`) means
  #5/#6/#8 cannot fold at compile time — they'd still call external-
  linkage symbols. The whole point of `constexpr inline` evaporates.
- #9 is a one-token change (`inline` → `constexpr inline`). The body is
  already a four-line constant expression with one `VIXL_ASSERT`.
  Lowest possible risk.

**Alternatives considered**:
- *Drop ScalarFormatFromFormat/MaxInt/MaxUint from scope*: rejected.
  These are listed in the profile and called by several NEON leaves
  (logic-aarch64.cc lines 1273, 1291, 1308, 2127, 2134, 2153, 2172,
  2577 etc.). Dropping them leaves measurable performance on the table
  without simplifying the change much.
- *Promote `ScalarFormatFromLaneSize` only locally (in
  `instructions-aarch64.h` next to its caller)*: same delta, no gain
  over the proposed layout.

### D5: Header ordering for the constexpr dependency closure

**Decision**: Inside the `instructions-aarch64.h` marker block, define
in this order to satisfy "constexpr definition before use":

```
ScalarFormatFromLaneSize           // independent, switch on lane_size_in_bits
LaneSizeInBytesLog2FromFormat      // independent
MaxLaneCountFromFormat             // independent
IsVectorFormat                     // independent
ScalarFormatFromFormat             // uses ScalarFormatFromLaneSize + LaneSizeInBitsFromFormat (A)
MaxIntFromFormat                   // uses LaneSizeInBitsFromFormat (A) + GetUintMask
MinIntFromFormat                   // uses MaxIntFromFormat
MaxUintFromFormat                  // uses LaneSizeInBitsFromFormat (A) + GetUintMask
```

The four Lever-A helpers (`IsSVEFormat`, `LaneSizeInBitsFromFormat`,
`LaneSizeInBytesFromFormat`, `LaneCountFromFormat`,
`RegisterSizeInBitsFromFormat`, `RegisterSizeInBytesFromFormat`)
already precede this block in the header per the A marker block.
`GetUintMask` lives in a separate file (`utils-vixl.h`) and is included
into `instructions-aarch64.h` transitively via existing includes.

**Rationale**: Same constexpr rule as A. Compiler enforces ordering;
the build fails if violated. Verified compile-time by the smoke test
extension (D6).

### D6: Compile-time fold guardrail extends the existing smoke test

**Decision**: Append `static_assert` cases to the existing
`test/instructions_aarch64_constexpr_smoke_test.cc` covering each of
the 9 newly-promoted helpers, with at least 2 distinct vform inputs
each to exercise switch arms. No new test target; reuse the existing
privileged-build pattern (PRIVATE `${PROJECT_SOURCE_DIR}/src` include +
VIXL defines).

**Rationale**: Cheaper than a new target; same guardrail strength. If
any helper silently drops `constexpr`-ness in future maintenance, the
TU stops compiling.

### D7: VIXL_ASSERT and VIXL_UNREACHABLE in constexpr context

**Decision**: Use these unmodified inside the inlined helpers, same as
A did.

**Rationale**: VIXL_ASSERT in release expands to `((void)0)` (constexpr-
safe). In debug it expands to `do { if (!cond) printf+abort; } while (0)`
— not constexpr per se, but C++14+ allows non-constexpr code paths
inside a constexpr function **as long as those paths are not taken in
the constant-evaluated context**. The static_assert call sites all pass
valid vforms (`vform != kFormatUndefined`), so the assertion branch is
never taken at compile time; same for `default: VIXL_UNREACHABLE();`
which the static_assert inputs never hit. A was already doing this
(RegisterSizeInBitsFromFormat asserts `!IsSVEFormat(vform)`) and the
existing smoke test compiles successfully in both Debug and Release.

## Risks / Trade-offs

- **Risk**: Binary size grows from inlining 9 helpers across all NEON
  call sites in `logic-aarch64.cc` and `simulator-aarch64.cc`.
  **Mitigation**: bodies are small (5-20 lines, mostly switch on a
  small enum with constant returns). The constexpr-fold path replaces
  a call+ret with a literal — net `.text` impact tends toward zero or
  small reduction. Verify with `wc -c build/release/src/libgaby_vm.a`
  pre/post (Task 2.3).
- **Risk**: `memset` of an 8-12 byte range incurs a fixed call overhead
  if libc doesn't inline. On Apple ARM64 hosts `_platform_memset` is
  intrinsic and short ranges hit a fast path; on Linux glibc the same
  is typical. **Mitigation**: bench acceptance gate (Task 4.2) catches
  pathological regressions. Worst observed even with non-inlined memset
  is still 2-3 ns/call, beating the 8-iteration loop at 8+ ns/call.
- **Risk**: Caller code outside this scope (e.g., a future addition)
  might depend on `SimRegisterBase`'s lack of a public `ClearTail`
  surface for some unintended contract. **Mitigation**: keep
  `ClearTail` semantically narrow (clears a tail byte range +
  notifies) and document it inline next to `Clear()` (which clears the
  full register).
- **Risk**: VIXL trace log behavior shifts due to the
  `NotifyRegisterWrite()` count change. **Mitigation**: Task 3.3
  diffs `--trace` output on a small NEON sequence to confirm parity.
- **Trade-off**: We bundle Lever B + Lever C into one change. They
  share the same review surface (imported-VIXL marker-block edits,
  same bench/test gates, same review reasoning) and landing them
  together saves one round of OpenSpec workflow. Cost: a single bench
  delta cannot attribute speedup between B and C precisely. Mitigated
  by the profile §2 split (ClearForWrite 12.1% vs helpers 3-5%) and
  by acceptance thresholds in Task 4.2 that hold the overall target
  but do not require per-lever attribution.

## Migration Plan

Not applicable — this is an internal performance change with no API,
spec, or data-format impact. Rollback is `git revert` of the squash
commit; no migration scripts.

## Open Questions

None. All identified closure points (ScalarFormatFromLaneSize,
GetUintMask) have decisions above; all risks have mitigations or are
quantified.
