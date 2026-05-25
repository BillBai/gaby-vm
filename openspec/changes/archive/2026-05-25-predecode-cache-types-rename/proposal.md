## Why

Three of the five public symbols in `include/gaby_vm/predecode_cache.h` are
mis-placed or mis-named for what the API has become.

1. **`RegisterStatus` collides head-on with the register-identity enums.** The
   archived `simulator-register-io-api` change (2026-05-24) added
   `gaby_vm::GpRegister`, `gaby_vm::VRegister`, and `gaby_vm::SysRegister` to
   the same namespace. Today three different things share a `Register…`
   prefix in `gaby_vm::`: the verb (`RegisterCodeRange`), the verb's outcome
   (`RegisterStatus`), and the CPU-register identity enums. A reader scanning
   `gaby_vm::Register*` can no longer tell action result from register name.
2. **`ErrorDetail` reads as a gaby-vm-wide error type but serves one method.**
   Its own doc comment notes it "mirrors errno / strerror" (design doc
   §4.3.2). Sitting at the `gaby_vm::` top level implies a global error
   vocabulary; in fact only `PredecodeCache::RegisterCodeRange` ever produces
   or consumes it.
3. **`PredecodedEntry` and `CodeRange` are PredecodeCache internals exposed
   only for hot-path inlining.** Their own doc comments say so verbatim
   ("public only so the cache-track execution path can perform its hot-path
   lookup inline"). Placing them at the namespace top suggests they are peers
   of `Simulator` and `PredecodeCache` — the namespace shape misleads.

The objective is a one-look public header: at the namespace level
`gaby_vm::PredecodeCache` and `gaby_vm::Simulator` are peers, and everything
that exists "only because `PredecodeCache` exists" lives inside the class.

## What Changes

All four type relocations are **BREAKING** changes to the V1 public API. No
compatibility aliases are introduced.

- **`gaby_vm::RegisterStatus` → `gaby_vm::PredecodeCache::RegistrationStatus`.**
  Renamed and nested. Underlying type, variant names (`Ok`, `InvalidArgument`,
  `OverlappingRange`, `UnsupportedFeature`, `OutOfMemory`), variant values,
  and the C-ABI integer shape are unchanged — only the scope and the
  spelling move.
- **`gaby_vm::ErrorDetail` → `gaby_vm::PredecodeCache::RegistrationError`.**
  Renamed and nested. Field set, field types, lifetime story, and POD-ness
  are unchanged.
- **`gaby_vm::PredecodedEntry` → `gaby_vm::PredecodeCache::PredecodedEntry`.**
  Nested only — same identifier inside its new scope. The 16-byte layout,
  the field set (`form_hash`, `reserved`, `leaf`), and the
  `static_assert(sizeof(PredecodedEntry) == 16, …)` are preserved verbatim;
  the assert moves with the type so the cache-track inline lookup ABI keeps
  its single-source-of-truth invariant.
- **`gaby_vm::CodeRange` → `gaby_vm::PredecodeCache::CodeRange`.** Nested
  only. Field set (`start`, `size_bytes`, `entries`) unchanged.
- **Method signatures follow.** `PredecodeCache::RegisterCodeRange` now
  returns `RegistrationStatus`. `GetLastErrorDetail` is renamed to
  `GetLastRegistrationError` so the method name and return type stay
  aligned, and returns `const RegistrationError*`. `FindRange` returns
  `const CodeRange*` (the now-nested type).
- **Marker-region pointer types in `src/aarch64/simulator-aarch64.h` follow
  the rename.** `cur_range_`, the `range` and `entry` locals in
  `ExecuteInstructionCached`, and `GabyInterpreterCursor::cur_range` become
  `const gaby_vm::PredecodeCache::CodeRange*` and
  `const gaby_vm::PredecodeCache::PredecodedEntry*`. The marker *reason* text
  — which cites the design doc — is not edited; only the C++ type names
  inside the marker block move.

### Naming choices considered

For `RegisterStatus`:

- `RegistrationStatus` ← **chosen.** Noun form of the action ("registration").
  Drops the `Register…` prefix entirely, so there is no textual overlap with
  `GpRegister` / `VRegister` / `SysRegister` left to confuse readers.
- `RegisterRangeStatus` — keeps the verb prefix; still scans as `Register…`
  next to the register-identity enums, so the original clash is only
  partially relieved.
- `CodeRangeRegistrationStatus` — accurate but long. Once the type lives
  inside `PredecodeCache`, the class scope already says what is being
  registered, making the prefix redundant.
- `PredecodeStatus` — narrows to the *act* of predecoding; the method that
  returns it is `RegisterCodeRange`, not `Predecode`, so the type would
  mismatch the method it serves.

For `ErrorDetail`:

- `RegistrationError` ← **chosen.** Pairs with `RegistrationStatus`. "Error"
  is a slight stretch — the struct's `status` field can also be `Ok`,
  meaning "no failure has been recorded" — but that is the type's resting
  value, not its documented role, which is the failure detail.
- `RegistrationFailureDetail` — sharpens "this is only for failures" but is
  longer and makes the resting `Ok` state read as a special case.
- `CodeRangeRegistrationError` — long; redundant once nested.

### Status and Error: nest, or keep at namespace top?

Both placements are viable; this proposal nests them, for the same reason
`PredecodedEntry` and `CodeRange` are being nested.

- **Nesting (chosen).** `RegistrationStatus` and `RegistrationError` exist
  *only* to describe `PredecodeCache::RegisterCodeRange`'s outcome, so
  collocating them inside the class matches how the API is actually used.
  Consistent with `PredecodedEntry` / `CodeRange`, so the header's rule
  becomes uniform: namespace top is for peers of `PredecodeCache`;
  everything cache-internal nests. Costs: longer fully-qualified spelling
  at call sites (`gaby_vm::PredecodeCache::RegistrationStatus::Ok`), and a
  forward declaration of the enum requires the full class definition (not
  just `class PredecodeCache;`).
- **Namespace top.** Shorter call-site spelling; enums commonly stay
  namespace-flat for FFI convenience. Re-introduces the "is this a
  gaby-vm-wide vocabulary?" ambiguity the rename is trying to settle,
  giving a half-fix.

Call-site verbosity is a minor concern in practice: only test and benchmark
code ever spells the type, and a local `using PredecodeCache::RegistrationStatus`
is available where it helps. The C-ABI underlying type (`int`) is unaffected
by nesting — only the C++ mangled name picks up the class scope.

### Non-Goals

- No semantic change to `PredecodeCache`: all-or-nothing registration, the
  append-only lifecycle, the shared-mutex / lock-free hot-path split, and
  the cross-thread concurrency model are all untouched.
- No change to `PredecodedEntry`'s 16-byte layout, field set, or
  `static_assert`. The cache-track inline lookup ABI is preserved.
- No new public methods, no removed methods beyond the
  `GetLastErrorDetail → GetLastRegistrationError` rename, no signature
  changes beyond return-type substitution.
- The public header stays vixl-free.
- Marker-region *reason* text in `src/aarch64/simulator-aarch64.h` is not
  edited — only the C++ type spellings inside the markers move.
- No compatibility aliases (`using OldName = NewName;`). V1 public API is
  still pre-stability; downstream code is updated in lockstep.

## Capabilities

### New Capabilities

*(none — this change relocates and renames types that `predecode-cache-core`
introduces.)*

### Modified Capabilities

- `predecode-cache`: the "RegisterCodeRange validates inputs and reports
  failures all-or-nothing" requirement names `RegisterStatus` and
  `ErrorDetail` by symbol. The delta updates those symbol names to
  `RegistrationStatus` and `RegistrationError` and applies the
  `PredecodeCache::` scope. The requirement *content* — the four non-`Ok`
  variants, all-or-nothing semantics, the queryable failure detail for
  `UnsupportedFeature` — is unchanged.

## Impact

- **Depends on `predecode-cache-core` being archived first.** That change
  creates the `predecode-cache` capability spec and ships the symbols being
  renamed. This proposal can be authored in parallel — its spec delta
  validates independently — but it must be archived after
  `predecode-cache-core` so the `predecode-cache` spec exists for the delta
  to apply against. The pattern mirrors `predecode-cache-benchmark`'s
  dependency on the same parent change.
- **Public header**: `include/gaby_vm/predecode_cache.h` — the rename + the
  nesting.
- **Implementation**: `src/gaby_vm/predecode_cache.cc` — every internal
  reference to the renamed types follows.
- **Imported tree marker region**: `src/aarch64/simulator-aarch64.h` — three
  pointer-type spellings inside the existing marker block:
  `Simulator::cur_range_`, the `range` / `entry` locals in
  `ExecuteInstructionCached`, and `GabyInterpreterCursor::cur_range`. Marker
  reasons remain verbatim.
- **Tests**: every `RegisterCodeRange` call site that spells the return
  type — `test/typed_register_io_test.cc`, `test/simulator_correctness.cc`,
  `test/reentrancy_test.cc`, `test/shadow_runner_test.cc`,
  `test/workload_shadow_test.cc`.
- **No imported VIXL semantic edits.** No new files. No new CMake targets.
  No new dependencies.
- **Reviewed but expected to be untouched** (mentioned for due diligence):
  `src/gaby_vm/simulator.cc` (one doc comment mentions
  `PredecodeCache::RegisterCodeRange` by name, which keeps its name);
  `include/gaby_vm/shadow_runner.h` and `src/gaby_vm/shadow_runner.cc`
  (forward-declare `PredecodeCache` and hold a pointer to it; do not name
  any of the renamed types); `bench/` (consumes only the higher-level
  `gaby_vm::Simulator` API and never spells these types).
- **C-ABI**: unchanged. `RegistrationStatus`'s underlying type stays `int`;
  nesting does not affect ABI for a C consumer reading a plain `int`.
- **Coding discipline**: per the archived
  `marker-block-style-and-brace-convention`, any single-line `if`/`for`
  statement touched during the rename must take `{}` and a new line. This
  is a project-wide rule, not a rename-specific one, but the rename will
  drag a few such lines through edits.
