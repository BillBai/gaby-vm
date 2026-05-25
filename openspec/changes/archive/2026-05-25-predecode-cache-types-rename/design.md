## Context

The V1 `include/gaby_vm/predecode_cache.h` exposes five top-level symbols in
`gaby_vm::`: `RegisterStatus`, `ErrorDetail`, `PredecodedEntry`, `CodeRange`,
and `PredecodeCache`. Three of them (`RegisterStatus`, `PredecodedEntry`,
`CodeRange`) advertise themselves as `PredecodeCache`-internal in their own
doc comments yet sit at namespace top, and `RegisterStatus` now textually
overlaps the `GpRegister` / `VRegister` / `SysRegister` family that
`simulator-register-io-api` (archived 2026-05-24) introduced in the same
namespace. The motivation, full-symbol mapping, and naming candidates are in
[`proposal.md`](proposal.md).

This design records the technical calls behind the rename: where each type
lands, what the method-name follow-through is, how the change sequences
against the in-flight `predecode-cache-core` archive, what changes inside the
imported-tree marker region, and the placement of the load-bearing
`static_assert`. None of it touches `PredecodeCache` semantics, the cache-track
inline ABI, or the imported VIXL tree.

## Goals / Non-Goals

**Goals:**

- One-look public header: `gaby_vm::Simulator` and `gaby_vm::PredecodeCache`
  are peers at namespace top; everything that exists *only because*
  `PredecodeCache` exists nests inside it.
- Eliminate the textual `Register*` collision between the cache action's
  return code and the CPU-register identity enums.
- Land as a pure rename + nesting — no semantic change, no ABI change for a C
  consumer, no edit to the cache hot path's inline layout.

**Non-Goals:**

- Any change to `PredecodeCache` behavior: all-or-nothing registration, the
  append-only lifecycle, the shared-mutex / lock-free hot-path split, and the
  cross-thread concurrency model stay exactly as `predecode-cache-core`
  delivered them.
- Any change to `PredecodedEntry`'s 16-byte layout, the field set, or what the
  cache-track inline lookup reads from it.
- Any deletion or addition of public methods beyond the
  `GetLastErrorDetail → GetLastRegistrationError` follow-through.
- Any compatibility aliases. V1 is pre-stable, every consumer is in-tree.
- Any edit to imported VIXL files outside the existing marker block, or any
  change to marker *reason* text.

## Decisions

### D1. All four affected types nest inside `PredecodeCache`

**Decision.** `RegisterStatus`, `ErrorDetail`, `PredecodedEntry`, and
`CodeRange` all become public nested types of `gaby_vm::PredecodeCache` —
`PredecodeCache::RegistrationStatus`, `PredecodeCache::RegistrationError`,
`PredecodeCache::PredecodedEntry`, `PredecodeCache::CodeRange`. No type
related to `PredecodeCache` remains at the `gaby_vm::` top level.

**Why.** Each of the four serves exactly one class: `RegistrationStatus` and
`RegistrationError` are the return surface of one method; `PredecodedEntry`
and `CodeRange` are the inline-lookup data the cache-track hot path indexes
through a `PredecodeCache*`. Nesting matches usage; namespace top would
re-advertise them as gaby-vm-wide vocabulary, which is the misread the rename
exists to fix. Doing all four together also makes the header's rule uniform —
*"namespace top is for peers of `PredecodeCache`; cache-internal nests"* — so
a reader does not have to remember case-by-case why one type is flat and
another is nested.

**Alternatives.**

- *Keep `RegistrationStatus` / `RegistrationError` at namespace top, nest only
  the two struct types.* Shorter call-site spelling for the enum and error;
  half-fix the namespace ambiguity. Rejected because the result is
  inconsistent — two types nested, two not, with no principled rule for the
  next maintainer to apply.
- *Keep all four at namespace top under different names (e.g.,
  `PredecodeRegistrationStatus`, `PredecodedEntry` …).* Drops the textual
  collision but still mislocates internal data as namespace-top vocabulary.

### D2. `RegistrationStatus` and `RegistrationError` are the chosen identifiers

**Decision.** `RegisterStatus` → `RegistrationStatus`; `ErrorDetail` →
`RegistrationError`. Variant names inside `RegistrationStatus` (`Ok`,
`InvalidArgument`, `OverlappingRange`, `UnsupportedFeature`, `OutOfMemory`),
their values, and the underlying type (`int`) are unchanged. The field set
of `RegistrationError` (`status`, `pc`, `reason`, `missing_features`) is
unchanged.

**Why.** `RegistrationStatus` is the noun form of the action `Register…
CodeRange`, so the type and the method that returns it read as a pair. It
drops the `Register…` prefix entirely, which is what the collision with
`GpRegister` / `VRegister` / `SysRegister` needs; `RegisterRangeStatus` keeps
the prefix and only partially relieves the clash. `CodeRangeRegistrationStatus`
is accurate but long and redundant inside `PredecodeCache::` scope.
`PredecodeStatus` would mismatch the method that returns it
(`RegisterCodeRange`, not `Predecode`).

`RegistrationError` pairs cleanly with `RegistrationStatus`. The struct's
`status` field can carry `Ok` when no failure has yet been recorded —
ostensibly inconsistent with "Error" in the type name — but that is the
resting state of a never-failed cache, not the type's documented role; the
struct is *the failure-detail record* even when it has none to report. The
alternative `RegistrationFailureDetail` is sharper but longer and complicates
the resting `Ok` reading; `CodeRangeRegistrationError` is verbose after
nesting.

### D3. `GetLastErrorDetail` becomes `GetLastRegistrationError`

**Decision.** The method `PredecodeCache::GetLastErrorDetail() const` is
renamed to `GetLastRegistrationError() const`. Return type follows
`ErrorDetail → RegistrationError`. Semantics — including "never returns null",
the pointee-validity lifetime, and the threading constraint — are unchanged.

**Why.** The method name encodes the type name; leaving it as
`GetLastErrorDetail` while it returns a `const RegistrationError*` is a
gratuitous mismatch readers will misread as two different concepts. The
method has no extra-textual contract pinned to the old name (no FFI export,
no embedder commitment at V1).

### D4. No compatibility aliases

**Decision.** No `using RegisterStatus = PredecodeCache::RegistrationStatus;`,
no `using ErrorDetail = PredecodeCache::RegistrationError;`, no
`[[deprecated]]` shims. The change updates every in-tree call site in
lockstep with the header.

**Why.** V1 is pre-stable and the audit (proposal Impact) shows every
consumer is in-tree: five test files name `RegisterStatus`, the imported-tree
marker block names `gaby_vm::CodeRange` and `gaby_vm::PredecodedEntry`, no
other file references the renamed types as symbols. A shim would have no
external consumer to protect and would re-introduce the namespace-top form
the change is trying to remove.

### D5. Sequence behind `predecode-cache-core`'s archive; author the spec delta in parallel

**Decision.** This change is implemented and archived *after*
`predecode-cache-core`. The `predecode-cache` spec delta is authored in
parallel against the post-archive spec text (the same text that currently
lives in `predecode-cache-core/specs/predecode-cache/spec.md`), validated
independently, and applied at archive time.

**Why.** `predecode-cache-core` ships both the live `predecode-cache`
capability spec and the symbols this rename targets. Archiving this change
first would require deltas against a spec that does not yet live in
`openspec/specs/predecode-cache/`. The dependency is the same shape as
`predecode-cache-benchmark`'s dependency on the same parent change. The spec
delta itself is mechanical (two symbol substitutions inside one
`MODIFIED Requirements` block), so authoring it in parallel is cheap; only
the *archive* step has to wait.

**Risk if the parent archive slips.** This change blocks on it. There is no
implementation pressure either way — the rename is a single contiguous PR
that can sit in branch until the parent archives.

### D6. Marker-region edits are pointer-type spellings only

**Decision.** Inside the existing `// gaby-vm BEGIN` / `// gaby-vm END` block
in `src/aarch64/simulator-aarch64.h`, three pointer-type spellings update:
`Simulator::cur_range_` (the cached-range field), the `range` and `entry`
locals in `ExecuteInstructionCached`, and `GabyInterpreterCursor::cur_range`.
They become `const gaby_vm::PredecodeCache::CodeRange*` and
`const gaby_vm::PredecodeCache::PredecodedEntry*` respectively. The marker
*reason* text — the prose that cites the design doc — is not edited. No code
moves into or out of the marker block.

**Why.** The marker-block convention treats the *reason text* as the
contractual justification for the marker's existence; rewording it implies a
new contract. The rename is a name change, not a contract change, so the
reason stays verbatim. Conversely, the C++ symbol names *inside* the markers
are part of the bracketed code and must update with the public header — a
stale `gaby_vm::CodeRange*` field type would not even compile.

### D7. `static_assert(sizeof(PredecodedEntry) == 16, …)` moves inside the class body

**Decision.** When `PredecodedEntry` nests inside `PredecodeCache`, the
`static_assert` moves with it: it lives as a class-body member, declared
immediately after `struct PredecodedEntry { … };` and before
`struct CodeRange { … }` (which references `PredecodedEntry`).

**Why.** The assert exists to lock the cache-track inline ABI — it pins the
struct's size. Adjacent placement to the struct keeps the invariant
single-source: a future editor adjusting the struct sees the assert in the
same eye-span. Class-scope `static_assert` has been valid C++ since C++11 and
needs no `;` after it. The alternative (file-scope assert with the qualified
name `sizeof(PredecodeCache::PredecodedEntry)`) works but separates the
invariant from the thing it constrains.

## Risks / Trade-offs

- **Verbose qualified spelling at test/bench call sites.** A test that today
  writes `gaby_vm::RegisterStatus::Ok` becomes
  `gaby_vm::PredecodeCache::RegistrationStatus::Ok`. → Use a local `using
  gaby_vm::PredecodeCache;` or `using RegisterStatus =
  gaby_vm::PredecodeCache::RegistrationStatus;` in test files where the
  spelling appears more than once. Only test code ever spells the type
  directly; production code uses it through the method return.
- **Forward-declaring a nested type is harder than a namespace-top type.** A
  consumer that wants to forward-declare `class PredecodeCache;` for a
  pointer-only reference still works unchanged (and that is what
  `include/gaby_vm/simulator.h` and `include/gaby_vm/shadow_runner.h` already
  do). A consumer that wanted to forward-declare `gaby_vm::CodeRange` or
  `gaby_vm::PredecodedEntry` would now need the full `PredecodeCache`
  definition. → No such consumer exists today; the only forward-declarers are
  the two listed headers, both of which only need the class itself.
- **Parent-archive slip blocks this change.** → No implementation pressure;
  branch sits until parent archives. The spec delta and the code can be
  finished and reviewed independently.
- **Variant identifiers in `RegistrationStatus` keep their old short names
  (`Ok`, `InvalidArgument`, …).** A reader scanning
  `PredecodeCache::RegistrationStatus::OverlappingRange` may briefly wonder
  if the variant set changed. → It did not. Keeping the variant set bit-for-bit
  identical preserves the C ABI (a C consumer reading the int gets the same
  numeric value for the same outcome) and avoids unnecessary churn at call
  sites that already pattern on the variant name.

## Migration Plan

Single contiguous PR. Sequence:

1. Edit `include/gaby_vm/predecode_cache.h`: nest the four types, apply the
   two renames, move the `static_assert`, rename `GetLastErrorDetail` to
   `GetLastRegistrationError`.
2. Edit `src/gaby_vm/predecode_cache.cc`: substitute every internal reference;
   no behavioral edits.
3. Edit the marker block in `src/aarch64/simulator-aarch64.h`: update the three
   pointer-type spellings; do not edit the marker *reason* lines.
4. Edit the five test files (`test/typed_register_io_test.cc`,
   `test/simulator_correctness.cc`, `test/reentrancy_test.cc`,
   `test/shadow_runner_test.cc`, `test/workload_shadow_test.cc`): substitute
   `gaby_vm::RegisterStatus` → `gaby_vm::PredecodeCache::RegistrationStatus`.
5. Build, run CTest, run `openspec validate predecode-cache-types-rename
   --strict`, then archive.

Rollback is `git revert` on a single commit. No on-disk state, no external
service, nothing to migrate.

## Open Questions

None blocking. The two implementation-time micro-decisions worth surfacing:

- *Where do test files put the `using` shorthand?* — File-scope `using` at
  the top of each test, anonymous-namespace `using`, or a per-function local.
  Picked at implementation time; not a public-contract question.
- *Should the `RegistrationError` `status` field's `Ok` resting value be
  re-asserted in the constructor?* The current `last_error_{RegisterStatus::Ok,
  0, "", ""}` already does this; the rename preserves the initializer.
