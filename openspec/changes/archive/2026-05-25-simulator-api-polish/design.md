## Context

`gaby_vm::Simulator` was introduced by the `predecode-cache-core` change.
Its public surface — Pimpl class in `include/gaby_vm/simulator.h`,
implementation in `src/gaby_vm/simulator.cc` — is fresh: not yet exposed to
any out-of-tree embedder, currently used by three in-tree consumers
(`ShadowRunner`, the workload harness, the `test/` suite). That window of
"public-but-not-yet-load-bearing" is the right moment to polish three things
whose individual cost (one rename, one constexpr + check, one comment block)
is too small for a standalone OpenSpec change.

Two facts shape the decisions below:

- The imported `vixl::aarch64::SimStack` defaults
  (`src/aarch64/simulator-aarch64.h:191-193`) are `base_guard_size_ = 256`,
  `limit_guard_size_ = 4 * 1024`, `usable_size_ = 8 * 1024`. The
  gaby-vm Simulator does NOT use VIXL's `SimStack` allocator — the comment
  in `simulator.cc:150-151` is explicit: "That buffer IS the guest stack;
  the SimStack the imported Simulator allocates for itself goes unused".
  So the VIXL defaults are a *reference*, not an enforced layout.
- All in-tree tests allocate a 16 KiB `StackBuffer`
  (`alignas(16) std::array<uint8_t, 16 * 1024>`) or larger (the workload
  shadow test uses 1 MiB). Nothing in-tree exercises a tiny stack.

## Goals / Non-Goals

**Goals:**

- Move the observer-payload struct name from a verb-shaped `MemoryWrite` to
  the noun-shaped `MemoryWriteEvent`, in one atomic in-tree rename.
- Give the constructor a real input-validation contract so a `stack_size = 0`
  bug fails at construction with a useful diagnostic, not at first guest
  store with a SIGSEGV in a leaf.
- Capture the "narrow `SetCPUFeatures` later, when the cache pre-screen
  starts mattering" follow-up as a code-resident TODO with enough context
  that whoever picks it up does not need to rediscover the rationale.

**Non-Goals:**

- Not the actual CPU-features narrowing. That needs an enumerated EL0
  user-mode subset and coordinated changes to both the debug-track auditor
  and the cache-track pre-screen — outside the scope of a polish change.
- Not a rename of `DivergenceReport::Kind::MemoryWrite` (`shadow_runner.h:41`)
  or of the imported `vixl::aarch64::MemoryWriteSink` interface. The former
  is a separate enum naming a *kind of divergence*, not the event payload;
  the latter is upstream VIXL surface (`src/aarch64/simulator-aarch64.h:1307`)
  and out of scope for project-side polish.
- Not an unwind/abort policy redesign. The existing accessor abort pattern
  (`std::snprintf` into a stack buffer, then `VIXL_ABORT_WITH_MSG`) is reused.
- Not a `MemoryWriteObserver` API redesign. The alias name and signature
  stay, only the payload struct moves.

## Decisions

### D1. Renamed struct: `MemoryWriteEvent`

The struct represents the data an observer receives when a guest memory write
fires. Three candidate names were considered:

| Candidate            | Why considered                                | Why not                                                                       |
| -------------------- | --------------------------------------------- | ----------------------------------------------------------------------------- |
| `MemoryWriteRecord`  | Pairs with `ForwardingWriteSink::Record(...)` | Bookkeeping connotation; the data is a one-shot notification, not a log entry |
| `MemoryWriteInfo`    | Common payload suffix                         | `*Info` is vague; doesn't telegraph "observer fires"                          |
| `MemoryWriteEvent`   | Pairs naturally with `MemoryWriteObserver`    | (Selected)                                                                    |

`MemoryWriteEvent` wins because the observer/event pair is the dominant idiom
across the C++ standard library and most embedder code, so the type tells the
reader the lifetime model immediately: it is a transient value passed by
const-ref into a `std::function`, with no ownership.

The `MemoryWriteObserver` alias keeps its name. Renaming it to
`MemoryWriteEventObserver` would stutter without adding meaning.

### D2. `kMinStackSize = 12 * 1024` (12 KiB), exposed as a public constexpr

The choice has two dimensions: the *value* and the *visibility*.

**Value.** Three candidates were considered:

| Candidate     | Argument for                                                                | Argument against                                                                                                  |
| ------------- | --------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| 4 KiB         | One host page on iOS/macOS/Linux ARM; comfortably above "obvious-bug" sizes | Too lenient — accepts buffers smaller than a single VIXL `SimStack` `limit_guard` (4 KiB) on its own              |
| 12 KiB        | Equals VIXL's reference `limit_guard + usable` total (4096 + 8192 = 12288)  | None for in-tree use (all tests pass 16 KiB+)                                                                     |
| 64 KiB        | Round, generous                                                             | Rejects legitimate small embedded tests; bigger than any forward use case we can name; sets an arbitrary ceiling-shaped floor |

12 KiB wins because it tracks the only quantitative reference the project
has — VIXL's own opinion of "a real-stack-shaped stack" — without inheriting
the unrelated `base_guard` (256 B). The number is documented as
"limit_guard + usable from `src/aarch64/simulator-aarch64.h:191-193`" so
later updates have a clear anchor. All in-tree tests use 16 KiB, so 12 KiB
leaves a 4 KiB cushion against future minor adjustments.

**Visibility.** Two candidates:

| Candidate                                    | Why                                                              | Why not                                                                                          |
| -------------------------------------------- | ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| File-static `constexpr` in `simulator.cc`    | Smallest surface; embedders rarely care about the exact number   | Diagnostic mentions "kMinStackSize" but embedders can't name it — clumsy when sizing a buffer    |
| Public `static constexpr` on `Simulator`     | Lets embedders write `std::array<uint8_t, Simulator::kMinStackSize>` directly | Adds one symbol to the public API                                                                |

Public wins: the diagnostic and the embedder's code now use the same symbol,
no magic number drift. The cost is one additional public member —
acceptable for a polish change that's already touching the public class.

### D3. Constructor check placement and diagnostic shape

The check happens **before** the `std::make_unique<Impl>(...)` call in the
constructor's initializer list, by routing the constructor through a small
helper that does the validation then constructs `Impl`. Failing inside `Impl`
would still abort, but at higher cost (the `Impl` allocation has already
happened) and from a less obvious file. A free helper `ValidateStackSize`
in the anonymous namespace of `simulator.cc` keeps the constructor body small
and matches the file's existing layout.

Diagnostic uses the existing pattern (`typed_register_io_test`-flavor):

```cpp
char msg[160];
std::snprintf(msg, sizeof(msg),
              "gaby_vm::Simulator: stack_size %zu is below the minimum "
              "kMinStackSize=%zu (see include/gaby_vm/simulator.h)",
              stack_size, Simulator::kMinStackSize);
VIXL_ABORT_WITH_MSG(msg);
```

Two values in the message, both named, plus a header pointer so a developer
hitting this for the first time knows where the constant lives.

### D4. TODO comment for `SetCPUFeatures(All())`

Three pieces of information must survive in the source so the follow-up is
actionable without re-reading the proposal:

1. **Why `All()` today.** The debug-track decoder runs the imported
   `CPUFeaturesAuditor`. The cache track's `RegisterCodeRange` will also
   pre-screen instructions through the same auditor. During V1 bring-up,
   correctness coverage runs across the full A64 baseline; a narrower set
   would reject legitimate baseline encodings the tests need.
2. **Why it must narrow.** Project scope is EL0 user-mode (`AGENTS.md`
   «Scope»). With `All()`, the cache-track pre-screen accepts non-user-mode
   encodings that cannot legitimately appear in a guest binary, wasting
   predecode effort on them. The narrowing is a tightening, not a feature.
3. **What the follow-up is.** A dedicated change that enumerates the
   user-mode feature subset, plumbs it through both tracks, and adds a
   diagnostic when an out-of-set encoding is encountered.

The TODO is a `// TODO(simulator-cpu-features):` tagged comment so a future
`git grep -n 'TODO(simulator-cpu-features)' src/` finds the single call site.
No tracking-issue URL is added (the project doesn't use issues for this kind
of follow-up); the kebab-case tag IS the lookup key.

## Risks / Trade-offs

- **Breaking rename, but no out-of-tree consumers** → mitigation: the change
  is atomic (all in-tree call sites flip in one commit), and the rename is
  called out as **BREAKING** in the proposal so the commit log captures it.
  Future external embedders will see the new name only.
- **`kMinStackSize` could be too aggressive for an unforeseen tiny-stack
  embedder use case** → mitigation: 12 KiB is half a page on most desktop
  hosts and four pages on a 4 KiB-page mobile host — large in absolute terms
  but small relative to any non-toy embedder workload. Loosening later is a
  one-line constant edit and a spec scenario update; tightening later would
  be much harder, so erring conservative is the right direction.
- **The TODO could rot** → mitigation: the kebab-tag (`simulator-cpu-features`)
  is unique, so the next change that touches the auditor finds it via grep.
  The follow-up is small enough that a separate tracking-issue layer would
  cost more than it earns.

## Migration Plan

In-tree only:

1. Land the rename + the `kMinStackSize` check + the TODO comment in one
   commit. The three changes are independent at runtime but share the same
   review surface (`simulator.h` + `simulator.cc`) and the same testing
   path (`ctest` over the existing suite + one new abort test), so bundling
   is cheaper than three commits.
2. No staging period. No external embedders exist; the in-tree consumers
   are updated in the same commit. Rollback is a single `git revert`.

## Open Questions

None. The choices above are firm.
