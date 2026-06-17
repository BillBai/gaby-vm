# `vixl_port` Live-Assembly Rewrite - Code Review Record - 2026-06-10

> **What this is:** a code-review archive for the
> `vixl-port-live-assemble-rewrite` work as it existed against `8859018`. The
> review combined three static-review angles, direct local builds in Debug and
> Release, and a new coverage build.
>
> **What this is not:** not a schedule, not an OpenSpec change, and not a
> rejection of the design. The design skeleton was sound. This record captures
> the real state of the guardrail on 2026-06-10, the known defects, evidence,
> and a prioritized action list.
>
> **Status:** review complete. The four Critical items were mandatory before
> treating this suite as a performance-change release gate.

## Executive Summary

The live-assembly rewrite fixed the largest structural hole in the old frozen
fixture model: memory semantics. Of 589 upstream `TEST` bodies, 485 ran for real
and passed all two-track/two-oracle checks, 104 were skipped with reasons, and
0 failed. Debug and Release results matched.

The isolation engineering was strong:

- no RWX behavior;
- no shipping dependency on the assembler island;
- no ODR leakage found;
- license boundaries looked clean;
- 21 copied island files matched upstream SHA `160c445` with no undocumented
  drift.

The main problem was narrower but important: a green result could lie. Four
paths could convert something that should be a failure into a skip, while the
CTest result stayed green. That undermines the suite's most important property:
when dispatch/cache work breaks execution, the guardrail must fail loudly.

The design rating was roughly 7/10 at review time, and likely 8.5-9/10 after the
four Critical fixes.

## 0. Mental Model

The suite compares three engines running the same ARM code:

- VIXL reference simulator: the standard answer.
- gaby-vm decoder track: `DebugRunFrom`.
- gaby-vm cache track: `RunFrom` through `PredecodeCache`.

The suite is strong at detecting dispatch/cache mistakes because the cache track
takes the optimized route while the decoder/reference tracks do not. It is
weaker at detecting shared leaf-semantics bugs because all three engines use the
same imported VIXL simulator leaf code.

The most independent truth source is the upstream literal assertion data, such
as fixed `ASSERT_EQUAL_64(...)` expectations in VIXL tests.

## 1. Measured Results

Debug and Release both had 17/17 CTest tests green. For the three `vixl_port`
families:

| Family | Upstream TEST bodies | Ran and passed | Skipped | Failed |
| --- | ---: | ---: | ---: | ---: |
| Total | 589 | 485 (82%) | 104 | 0 |

The 104 skips were all categorized:

- feature deny list: 24;
- named quarantine: 22;
- no `RUN()`: 10;
- FP16 assertion coverage gap: 24;
- signal fallback: 7;
- reference-vs-literal mismatch: 8;
- cache registration refused: 5;
- instruction limit: 4.

Runtime was cheap enough for frequent local use: roughly 7.5 seconds in Debug
and 0.39 seconds in Release on the reviewed machine.

## 2. Coverage

Coverage was measured with a separate coverage build without source changes.
For the `vixl_port` families driving `Sources/gaby_vm`:

| Scope | Region | Function | Line |
| --- | ---: | ---: | ---: |
| `Sources/gaby_vm` overall | 40.2% | 53.8% | 52.4% |
| `simulator-aarch64.cc` | 31.0% | 33.5% | 40.0% |
| `simulator-aarch64.cc` excluding SVE | 49.4% | - | 63.7% |
| `predecode_cache.cc` | 80.3% | 81.3% | 61.2% |
| `logic-aarch64.cc` | 50.4% | 67.0% | 55.1% |

The raw simulator coverage is dragged down heavily by SVE, which is outside the
current project scope. Excluding SVE, only a small set of non-SVE leaf families
remained unvisited: trap/exception-style leaves, crypto leaves, and one FP16
NEON misc leaf that matched the FP16 skip category.

For differential tests, line coverage has stronger meaning than ordinary unit
coverage: a covered leaf line ran through multiple engines and the final state
was compared.

## 3. Findings

### Critical

**C1. Crashes in the gaby tracks were recorded as skips instead of failures.**

If a cache-path bug jumps to the wrong address, reads out of range, or loops
forever, the OS sees SIGSEGV or SIGALRM. The harness treated those as
`skip: aborted during run`, so CTest stayed green. That is the exact failure
class expected from aggressive dispatch work and should be a hard failure.

The signal path also used `siglongjmp`, skipping C++ destructors and leaving the
engine's busy latch potentially stuck for later cases.

Fix direction: record which phase is running, make gaby-track signals fail the
family, and rebuild or stop using the damaged engine state.

**C2. Guarded page state leaked from one reference-simulator case into later
cases.**

`START()` reset many simulator states but did not reset guarded pages. BTI cases
could leave the reused reference simulator in guarded mode, causing later branch
tests to be skipped before the gaby tracks ran.

Observed victims included tagged-pointer branch cases, which are relevant on
iOS. Fix direction: call `SetGuardedPages(false)` during reset.

**C3. `ASSERT_EQUAL_64` on vector/floating registers read the integer register
bank.**

VIXL overloads distinguish `X17` from `D17`. The rewritten harness used a
duck-typed "has `GetCode()`" rule and treated both as the same integer register
number. As a result, assertions such as `ASSERT_EQUAL_64(..., d17)` read `x17`,
causing the case to be skipped as a reference/literal mismatch.

Fix direction: identify V registers and read `sreg_bits`, `dreg_bits`, or vector
halves instead of the X bank.

**C4. The suite did not assert ran/skip baselines.**

The family main returned failure only when `failed > 0` or no cases existed.
If a regression moved many cases from "ran" to "skipped", the suite still passed.
This had already happened through C2.

Fix direction: add expected ran/skip counts per family, or compare against
committed manifests with an explicit rebaseline path.

### Important

- FP16 assertions were all skipped even though the leaf implementation exists.
  Add FP16 raw-bit support in the reference-state view and assertion checker.
- `tasks.md` contained inconsistent verification numbers; use the measured
  numbers from this review.
- `ASSERT_EQUAL_MEMORY` comments and proposal wording overstated what the macro
  checked. Memory semantics were covered by the frame-window oracle instead.
- The frame-window mechanism had silent degradation paths and no smoke test with
  a real store. Add an RMW smoke case and warn when the frame window is
  inactive.
- Exclusive-monitor PRNG seed state was not reset across all tracks; document
  or reset it.
- Unrelated dispatch/benchmark docs and generated HTML were mixed into the
  change. Split them.
- `AssertEntryEquivalentOnce` checked only `cache_sim`; add the equivalent
  check for `decoder_sim`.
- Literal-pool/data-in-stream cases showed a real product gap: cache
  registration rejected ranges containing inline data and therefore prevented
  even decoder-track replay.

### Minor

- A few marker-style deviations from `docs/conventions.md`.
- Missing quarantine entries in `docs/testing.md`.
- A large harness header should eventually split by responsibility.
- Signal handling lacked `sigaltstack`.
- Single-case triage switches such as `VIXL_PORT_ONLY` would improve debugging.
- `system_rng` consistency depended on synchronized seeds and deserved a
  comment.

## 4. Design Assessment

The three-engine design is sound for its primary purpose:

- Strong for dispatch/cache regressions.
- Weak for bugs shared by imported VIXL leaf semantics.
- Most independent truth comes from literal upstream assertions.

Copying the VIXL assembler island instead of linking `../vixl` at build time was
the right choice. Linking a local `../vixl` tree would make the required SHA a
per-machine state and risks ODR problems between copied and modified headers.

Rewriting assertion macros was also reasonable because upstream assertion
failures abort the process, but it meant the harness had to faithfully preserve
VIXL's overload distinctions. C3 was the direct cost of missing that detail.

Lockstep cosimulation should not be the primary oracle because exclusive monitor
randomness can create mid-body false differences, but it is a good failure
diagnostic for cache vs decoder. On a differential failure, rerun that case
step-by-step and report the first divergent PC.

The frame-window memcpy approach solved a real problem: tests with stack-local
memory operands need each engine to see a clean memory image. However, copying a
live C++ stack frame depends on frame-pointer behavior, local placement, and no
ASan. A cleaner mid-term replacement is the existing `MemoryWriteSink`: record
writes per engine, compare write records, and undo changes precisely.

## 5. Action List

| Priority | Work |
| --- | --- |
| Before merge | Normalize `tasks.md` numbers, fix memory-assert wording, and split unrelated dispatch/benchmark artifacts. |
| P0 before using as a perf gate | Fix C1 crash classification and busy latch; reset guarded pages; split GP vs V register assertions; assert ran/skip baselines. |
| P1 | Add FP16 assertions; add frame-window RMW smoke and inactive warning; compare full gaby/ref register files at the end; seed-check decoder sim; expose reference-mismatch skip counts. |
| P2 | Add automatic lockstep rerun and slice dumps on failure; add triage switches; document monitor seed behavior; add crypto point tests; evaluate data-in-stream fallback. |
| Mid-term | Replace frame-window memcpy with `MemoryWriteSink`; split the large harness header; copy the upstream macro header for upgrade diffs; evaluate porting `test-simulator-aarch64.cc`. |

Bottom line: the guardrail had the right foundation. It closed the memory
semantic blind spot, kept the assembler island isolated, and ran fast enough for
regular use. Before the Critical fixes, though, `ctest -R vixl_port` being green
was not sufficient evidence for risky performance work.

## 6. Index

- Reviewed design:
  [`gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md`](../../refs/gaby-vm-vixl-port-live-assemble-rewrite-2026-06-09.md)
  and the `openspec/changes/vixl-port-live-assemble-rewrite/` change.
- Suite docs:
  [`../testing.md`](../../testing.md),
  [`../architecture.md`](../../architecture.md),
  [`vixl-extraction-map.md`](../../refs/vixl-extraction-map.md), and
  [`AGENTS.md`](../../../AGENTS.md).
- Core implementation areas at review time:
  `test/test_support/vixl_asm/harness/gaby_two_track_macros.h`,
  `test/test_support/vixl_asm/harness/gaby_two_track_main.h`,
  `test/test_support/vixl_asm/harness/vixl_port_oracle.{h,cc}`,
  `test/test_support/vixl_asm/CMakeLists.txt`, and
  `Sources/gaby_vm/src/gaby_vm/predecode_cache.cc`.
