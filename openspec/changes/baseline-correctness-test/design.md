## Context

The `extract-vixl-aarch64-simulator-baseline` change brought VIXL Tiers 1+2+3
into `src/` and added two test executables: `gaby_vm_smoke` (proves the
project's static library links and reports a version) and `simulator_smoke`
(proves a `vixl::aarch64::Simulator` constructs and steps through a single NOP
via `WritePc` + `ExecuteInstruction`). Neither test asserts that the simulator
*computed the right answer* — `simulator_smoke` does not even check the post-NOP
PC. The `aarch64-simulator` capability spec explicitly owns the test contract
(its R9 and R10), so the contract is the right surface to extend.

This change adds a third executable, `simulator_correctness`, that drives
hand-encoded instruction sequences through `Simulator::RunFrom` and asserts on
post-run register and memory state. The proposal already pins the coverage
matrix (integer arithmetic, logical, load/store, conditional branches, BL/RET);
this design pins down *how* — the harness shape, the instruction-encoding
discipline, the termination protocol, the assertion machinery, and the CMake
wiring — without committing to specific encodings (those belong in the source).

Architectural facts worth front-loading because they constrain everything:

- `Simulator::ResetRegisters()` (invoked from the constructor's `ResetState`)
  sets `LR = kEndOfSimAddress`, where `kEndOfSimAddress == nullptr`. The main
  `Simulator::Run()` loop exits when `pc_ == kEndOfSimAddress`. Concretely:
  **a sequence terminates by `RET`-ing to a NULL link register.** Every
  sequence we author is, by construction, a leaf function body.
- `Simulator::RunFrom(const Instruction* first)` is a two-liner — it calls
  `WritePc(first, NoBranchLog)` then `Run()`. There is also a templated
  `RunFrom<R, Args...>` that wraps argument marshalling and a return-value
  read. The templated form is gated on `VIXL_HAS_ABI_SUPPORT` and explicitly
  TODOs stack-argument passing; we will not depend on it.
- The `Simulator` constructor takes `(Decoder*, FILE* stream = stdout,
  SimStack::Allocated stack = SimStack().Allocate())`. By default it allocates
  a stack and points SP at it, so SP-relative load/store works out of the box.
- The CPU-features auditor assertion inside `ExecuteInstruction()` will fire
  on most non-trivial instructions unless the auditor is configured. The same
  fix `simulator_smoke` already uses — `sim.SetCPUFeatures(CPUFeatures::All())`
  — is the intended remedy.

Project constraints carried over from `CLAUDE.md` and the import-baseline
change:

- iOS / macOS / POSIX portable, **no JIT and no RWX memory.** Encoded
  instructions live in ordinary host data memory, accessed via
  `reinterpret_cast<const Instruction*>(...)`.
- No assembler is available. Encodings are hand-built `uint32_t` literals.
- Imported `src/` files MUST NOT be modified by this change. Test-side code
  follows the project's normal warning policy via
  `gaby_vm_apply_compile_flags`, not the relaxed imported-source policy.

## Goals / Non-Goals

**Goals**

- Define the shape and lifecycle of `simulator_correctness`: a single
  translation unit, family-grouped sub-tests, hand-rolled assertion harness,
  no new external dependency.
- Define how raw `uint32_t` instruction sequences are encoded, documented, and
  protected against transcription error.
- Define the harness boundary with VIXL: which Simulator API surface we lean
  on, and which we deliberately avoid.
- Define a load/store memory model that works under the no-RWX, no-JIT
  constraint: the simulator is a software interpreter, so test data lives in
  ordinary host buffers reached through register-held addresses.
- Settle the BL/RET sequence-shape question (saving/restoring LR around the
  inner call so the outer `RET` still terminates the simulation).
- Define the CMake wiring delta and the executable / CTest naming, mirroring
  the existing `simulator_smoke` block as closely as practical.

**Non-Goals**

- A general assertion library, parameterised test runner, or fixture
  framework. The assertion surface is tiny; a hand-rolled `expect_eq` plus a
  failure counter is sufficient.
- Helper macros that build instruction words from operand fields. Those are
  an in-tree assembler in disguise — Tier 0 by another name — and undermine
  the auditability that hand-encoded literals provide.
- Importing or generating VIXL-style instruction-emit helpers (e.g.,
  `Assembler::Add(...)`). Same Tier-0 reason.
- Any change to imported `src/` files, public headers under `include/gaby_vm/`,
  exported CMake targets, or external dependencies.
- SIMD / SVE / FP / pointer-auth coverage. Deferred per the proposal.
- A negative-test surface (e.g., asserting that an undefined encoding traps).
  Out of scope for the integer-correctness baseline.

## Decisions

### D1. One translation unit; family-grouped sub-tests; hand-rolled assertions

**Decision.** A single source file, `test/simulator_correctness.cc`, contains:

- An anonymous-namespace `TestState` struct holding a failure counter and the
  currently-running test name (used in failure messages).
- A small assertion API in the same anonymous namespace:
  - `expect_eq_u64(uint64_t actual, uint64_t expected, const char* label)`
  - `expect_eq_u32(uint32_t actual, uint32_t expected, const char* label)` (for
    32-bit register / memory reads)
  - `expect_eq_ptr(const void* actual, const void* expected, const char* label)`
- One free function per instruction family:
  `run_arithmetic_tests(TestState&)`, `run_logical_tests(TestState&)`,
  `run_loadstore_tests(TestState&)`, `run_branch_tests(TestState&)`,
  `run_call_return_tests(TestState&)`.
- A `main()` that calls each in turn, prints a final summary
  (`simulator_correctness: <n>/<m> sub-tests passed`), and returns 0 iff every
  sub-test passed.

There is exactly **one CTest case**, `simulator_correctness`, registered with
`add_test(NAME simulator_correctness COMMAND simulator_correctness)`. Failure
of any sub-test fails the whole test case; the printed summary makes which
sub-test failed obvious from CTest's captured output.

**Why.**
- Matches the existing `simulator_smoke` pattern (one TU, one CTest case,
  no test framework). The project has no Catch2 / GoogleTest dependency and
  this change is too small to justify introducing one — a forcing function
  worth respecting per `CLAUDE.md`'s "do not avoid necessary changes, but
  also do not invent abstractions".
- Family grouping at the function level keeps related encodings, fixtures,
  and assertions visible together. A reader investigating a failure jumps
  straight to one function rather than navigating a fixture tree.
- A single CTest case keeps CI configuration unchanged. Per-family CTest
  cases would require either separate executables (more CMake) or a CTest
  driver script (more layers); neither pays for itself at this size.

**Alternatives considered.**
- Per-family executables (`simulator_correctness_arith`, `…_logical`, etc.).
  Rejected: 5× the CMake boilerplate, 5× the process-startup cost in CI, no
  isolation benefit beyond what `Simulator` reset already gives us.
- Adopting a third-party test framework (Catch2, doctest, GoogleTest).
  Rejected: a real dependency for a 200-line test surface. Revisit later if
  the test count climbs into the dozens.
- A driver script that parses the binary's stdout to map sub-tests to CTest
  cases. Rejected: parsing layer adds fragility for cosmetic gain.

### D2. Instruction sequences as inline `alignas(uint32_t)` `uint32_t[]` literals, one mnemonic per word

**Decision.** Each sub-test holds its sequence as an `alignas(uint32_t)
uint32_t code[] = { … };` array, declared local to the sub-test function. Every
element is on its own line with an end-of-line comment giving the AArch64
mnemonic and operands the encoding represents:

```cpp
alignas(uint32_t) uint32_t code[] = {
    0x8b020020,  // add x0, x1, x2
    0xd65f03c0,  // ret
};
```

The harness obtains the entry point via
`reinterpret_cast<const Instruction*>(&code[0])` — the same cast pattern
`simulator_smoke` already uses, validated against the comment in
`simulator_smoke.cc:30-32` ("zero-byte wrapper over an instruction word, so
reinterpret_cast over a 4-byte-aligned buffer is the documented way…").

Every encoding in the test source is **separately verified against an external
assembler** (e.g. `llvm-mc`, `clang -c`, or upstream VIXL's assembler in
`../vixl`) **before commit**. The verification procedure — and the exact
external tool used — is documented as a comment block at the top of the file
so a reader knows how to reproduce it during re-syncs or audits.

**Why.**
- The Tier-0 boundary forbids importing an assembler; the proposal's
  "no-helper" line is firm. Inline raw words plus mnemonic comments is the
  honest expression of "we hand-encoded these".
- One word per line with a mnemonic comment is the established VIXL-test
  convention (their assembler-driven tests still print mnemonics in
  diagnostics) and is cheap to read during failure investigation.
- `alignas(uint32_t)` matches `simulator_smoke`'s setup (`alignas(4) uint32_t
  code = …`); 4-byte alignment is required for the `Instruction*` cast.
- External-tool verification before commit is the closest substitute for the
  type-safety an in-tree assembler would give us. Doing it once at
  authorship time is much cheaper than every CI run.

**Alternatives considered.**
- Helper macros like `ADD(Xd, Xn, Xm)` that compute encoding bits at
  compile time. Rejected: an assembler in 30 lines is still an assembler;
  it would be a Tier-0 import by another name, and would shift error from
  "wrong encoding, caught by external check" to "wrong helper math, caught
  by nothing".
- Loading encodings from a binary blob generated by an external assembler at
  build time. Rejected: introduces a host-toolchain dependency on an
  AArch64-capable assembler, which is exactly the iOS portability snag we
  want to avoid in test sources.
- Using VIXL's reference assembler at `../vixl` to generate the literals
  programmatically and embedding the output. Acceptable as a dev-side
  *verification* tool (D2 already calls this out) but inappropriate as a
  *runtime* dependency of the test binary.

### D3. Termination by trailing `RET`; rely on the implicit `LR == NULL` initial state

**Decision.** Every encoded sequence ends with the `RET` encoding
(`0xd65f03c0`). The harness invokes `sim.RunFrom(start)`; control returns
when the simulator's internal `Run()` loop observes `pc_ == kEndOfSimAddress`.
The harness performs **no manual `WriteLr` setup** — the implicit
`LR == nullptr` set by `ResetRegisters()` at construction is the contract we
rely on. If a test ever needs to call `RunFrom` more than once on the same
`Simulator`, it must call `ResetState()` in between (or, preferably, construct
a fresh `Simulator`; see D5).

**Why.**
- This *is* the protocol VIXL ships. Mirroring it (rather than inventing a
  custom termination scheme like a HLT trap) keeps the test honest about
  what the simulator does end-to-end.
- The behavior is documented inline in the upstream code:
  `Simulator::ResetRegisters` writes `LR = kEndOfSimAddress`,
  `IsSimulationFinished` checks `pc_ == kEndOfSimAddress`. No magic.
- Trailing `RET` doubles as a smoke-test of the `RET` leaf semantics on
  every sub-test, which is a pleasant accident of construction.

**Alternatives considered.**
- A custom termination instruction (`HLT #imm`, `BRK`) with the harness
  catching the resulting `Halt` callback. Rejected: requires touching
  imported code or the auditor to register a callback; trailing `RET` is
  already free.
- An infinite `b .` loop with the harness counting instructions and aborting.
  Rejected: needs to override `ExecuteInstruction` or instrument the loop;
  changes upstream behavior or imports new abstractions.

### D4. Manual `WriteXRegister` / `ReadXRegister` boundary; **do not** use templated `RunFrom<R, Args...>`

**Decision.** Each sub-test sets up inputs by direct register writes
(`sim.WriteXRegister(n, value)` for 64-bit, `sim.WriteWRegister(n, value)` for
32-bit), drives the sequence through the **non-templated** `sim.RunFrom(start)`,
and reads outputs by direct register reads (`sim.ReadXRegister(n)`,
`sim.ReadWRegister(n)`). The templated `RunFrom<R, Args...>` is **not used**.

For load/store tests, the host buffer's address is written into the chosen
base register (e.g., `sim.WriteXRegister(0, reinterpret_cast<uint64_t>(buf));`)
before `RunFrom`, and the simulator accesses the buffer through that
register-held pointer.

**Why.**
- The templated `RunFrom<R, Args...>` is gated on `VIXL_HAS_ABI_SUPPORT` plus
  a C++11+ check plus a GCC-version check; depending on it makes the test
  silently disappear on toolchains that fail any of those gates. The
  non-templated `RunFrom` has no such gates.
- The template only handles register-passed args (its source has an explicit
  TODO that stack-argument passing is unsupported). Future tests that need
  more than 8 inputs would have to drop down to the manual API anyway; using
  it uniformly avoids a split.
- The template returns exactly one value through the ABI return register.
  Manual reads can inspect any register, which we need for tests where the
  observable result is, say, an updated NZCV plus three written-through GPRs.
- Manual read/write also lets us inspect pre-`RET` state on failure if we
  ever extend the harness with a step-mode debug helper (deferred; flagged
  in Open Questions).

**Alternatives considered.**
- Templated `RunFrom` for the simple cases, manual for complex. Rejected:
  inconsistency increases reader friction; the manual API is not noticeably
  more verbose.
- A thin project-side wrapper that mimics the template but without its
  gating macros. Rejected: a wrapper for a one-line API is over-engineering.

### D5. Fresh `Simulator` per sub-test for state isolation

**Decision.** Each sub-test (each leaf function within a family — e.g. each
ADD/SUB/MUL exercise — not each family as a whole) constructs its own local
`Simulator` and `Decoder`. After the sub-test returns, both go out of scope
and are destroyed.

**Why.**
- `Simulator::ResetState()` exists and would be sufficient, but constructing
  a fresh instance is *both* simpler in test code (no risk of forgetting a
  reset) and cheaper in reasoning (no shared state at all between sub-tests).
- The constructor cost is the same setup `simulator_smoke` already pays once
  per process; paying it ~20× more is invisible in CI.
- If any sub-test ever leaves the simulator in a state `ResetState` doesn't
  fully clear (a real concern given how much state lives behind that struct),
  the fresh-construction approach insulates every later sub-test from the
  fallout.

**Alternatives considered.**
- One `Simulator` per family, `ResetState()` between sub-tests. Rejected:
  trades a tiny perf gain (constructor avoidance) for sharper failure modes
  if reset coverage drifts in a re-sync.
- One `Simulator` for the whole binary. Same objection, more strongly.

### D6. Load/store memory model: ordinary host buffer, address handed via X-register

**Decision.** Load/store sub-tests allocate a fixed-size, suitably-aligned
host buffer on the stack (e.g. `alignas(uint64_t) std::array<uint8_t, 64>
buf{};`), prefill it with a known pattern, write `&buf[0]` into the chosen
base X-register (commonly X0), and use immediate or register-offset addressing
modes against that base. After `RunFrom`, the harness asserts on:
- the post-state of any register the sequence loaded into (`ReadXRegister` /
  `ReadWRegister`), and
- the post-state of any byte the sequence stored into (read directly from the
  host buffer).

The buffer address is treated as an opaque integer when written to a register
(`reinterpret_cast<uint64_t>(buf.data())`); the simulator dereferences through
the register-held pointer using its standard memory primitives.

**Why.**
- The simulator is a software interpreter. There is no guest virtual address
  space to map; "memory" is whatever the simulator's leaf code ultimately
  dereferences. VIXL's load/store leaves dereference the register-held
  address directly into the host process's address space.
- Stack allocation keeps the buffer's lifetime tied to the sub-test; no
  manual cleanup, no risk of cross-test bleed.
- `alignas(uint64_t)` is sufficient for any address mode used in V1
  (single-register LDR/STR / register-offset). If a later test exercises
  unaligned-access checks, a separate buffer or alignment override is fine
  to add then.

**Alternatives considered.**
- A dedicated `SimStack`-style allocation. Rejected: the existing default
  `SimStack` already covers stack-relative addressing the simulator does
  internally; manual buffers are simpler than reusing internal infrastructure
  for explicit-address tests.
- Heap allocation with explicit `delete`. Rejected: lifetime overhead for no
  benefit at sub-test scope.
- Mapping a separate page with `mmap` or guard pages. Rejected: not portable
  across iOS, and not needed for correctness coverage at this layer.

### D7. BL+RET sequence shape: prologue saves LR to X19, epilogue restores

**Decision.** The BL/RET sub-test's outer sequence wraps its inner `BL` in a
two-instruction prologue/epilogue that preserves `LR`'s NULL terminator value
across the call:

```text
    mov   x19, lr        ; save outer LR (== kEndOfSimAddress) into a callee-saved reg
    bl    callee         ; corrupts LR with the return address of the next instr
    mov   lr,  x19       ; restore outer LR before the outer RET
    ret                  ; outer RET — terminates the simulation
callee:
    ...                  ; inner work that observably depends on having been called
    ret                  ; inner RET — returns to the instruction after BL
```

The *observable* check inside the callee should be a register write that the
outer code does not perform — e.g., the callee writes a sentinel to X0. Two
register reads after `RunFrom` then verify both legs:

- X0 holds the sentinel → `BL` correctly transferred control to the callee
  AND the inner `RET` correctly returned to the prologue's continuation.
- The simulation actually terminated (we got back from `RunFrom` at all) →
  the outer `RET` to restored `LR == NULL` worked.

**Why.**
- `BL` is defined to write the return address into `LR`, so the implicit
  `LR == NULL` terminator is destroyed by any inner call. Some restoration
  is required to terminate the simulation through `RET`.
- Saving to `X19` rather than to memory keeps the sub-test self-contained
  (no buffer needed) and exercises an additional standard-ABI register.
- The "callee writes sentinel" check is the smallest observable that proves
  *both* directions of the call worked, separating BL+RET correctness from
  arithmetic-leaf correctness which already has its own family.

**Alternatives considered.**
- Save LR to memory via `STP X29, X30, [SP, #-16]!` / `LDP X29, X30, [SP],
  #16`. Rejected for V1: requires SP arithmetic and pair load/store, neither
  of which is in the proposal's coverage matrix. We can layer that test in
  later without rewriting this one.
- Use the templated `RunFrom<...>` which sets LR=NULL on every call internally.
  Rejected per D4 — the template is gated on macros and limits which
  registers we can inspect.
- Skip BL+RET in V1. Rejected: the proposal explicitly includes it, and
  function call/return is the foundational control-flow primitive that
  every higher-level VIXL test will rely on.

### D8. Conditional branches: `B.cond` exercises NZCV via a preceding `SUBS`

**Decision.** The conditional-branch sub-tests cover three encodings:

- **`CBZ` / `CBNZ`** — flag-independent. The sub-test loads a known value
  into Xn, encodes a `CBZ Xn, +offset` (or `CBNZ`), and asserts on the
  post-run register state of a "marker" register that is written either side
  of the branch target. Two paired sub-tests cover taken vs. not-taken.
- **`B.cond`** — flag-dependent. The sub-test issues a `SUBS Xn, Xa, Xb`
  (subtract-and-set-flags) with operands chosen to produce a specific NZCV
  result, then a `B.cond <flag>, +offset`, and asserts on the marker
  register. Two paired sub-tests cover the condition true and false cases.

For `B.cond`, the choice of preceding instruction is **`SUBS`** (or an
equivalent `CMP`, which is just `SUBS` to `XZR`). `ADDS` would also work but
`SUBS`/`CMP` is the canonical condition-setter and keeps the test recognizable
to anyone who has seen AArch64 calling conventions.

**Why.**
- `B.cond` reads NZCV; without a preceding flag-setting instruction the
  flags are whatever `ResetSystemRegisters` initialized them to. Relying on
  that default value would couple the test to an implementation detail of
  the simulator's reset path; preceding it with an explicit `SUBS` makes the
  test self-determining.
- Pairing taken/not-taken sub-tests for each conditional encoding catches
  the most common encoding-error mode (sign of the immediate, condition-code
  field swapped, etc.) for the same authorship cost as covering one direction.
- `CBZ`/`CBNZ` is split out from `B.cond` because the encoding fields and the
  decoder paths differ; conflating them would obscure which encoding broke
  on a regression.

**Alternatives considered.**
- Use `ADDS` to set flags. Acceptable, but `SUBS`/`CMP` is more idiomatic.
- Cover only the taken direction. Rejected: leaves false-negative risk on
  the not-taken decoder path that the test was supposed to catch.
- Test `BR Xn`, `BLR Xn`, `RET Xn` (other-than-LR), or `B` (unconditional).
  Out of scope for V1 — flag for a follow-up if branch-encoding coverage
  becomes a known gap.

### D9. Executable and CTest case both named `simulator_correctness`

**Decision.** The new executable target is named `simulator_correctness`
(no `_test` suffix). The CTest case is registered with the same name:
`add_test(NAME simulator_correctness COMMAND simulator_correctness)`.

**Why.**
- Matches the existing `simulator_smoke` and `gaby_vm_smoke` naming exactly
  — both use a single name shared by the executable and the CTest case.
  The proposal text used `simulator_correctness_test` only as an example
  (`e.g.`); the design picks the convention-conformant name.
- The `_test` suffix would be the only test-target naming asymmetry in the
  project. Consistency wins for a foundational test that other contributors
  will model future tests on.

**Alternatives considered.**
- `simulator_correctness_test` (the proposal's example). Rejected — see why.
- `correctness_test` (drop the `simulator_` prefix). Rejected — there is
  more than one possible correctness surface (e.g., predecode-cache
  correctness, embedding-API correctness); the prefix anchors which one this
  is.

### D10. CMake wiring: copy `simulator_smoke`'s block verbatim, change names

**Decision.** `test/CMakeLists.txt` gains a third block immediately after the
existing `simulator_smoke` block, structurally identical:

```cmake
# Simulator correctness: drives hand-encoded AArch64 sequences through
# Simulator::RunFrom and asserts on post-run register/memory state. Same
# privileged build pattern as simulator_smoke (see comment above).
add_executable(simulator_correctness simulator_correctness.cc)
target_link_libraries(simulator_correctness PRIVATE gaby_vm::gaby_vm)
target_include_directories(simulator_correctness PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(simulator_correctness PRIVATE
  VIXL_INCLUDE_TARGET_A64
  VIXL_INCLUDE_SIMULATOR_AARCH64
  $<$<CONFIG:Debug>:VIXL_DEBUG>)
gaby_vm_apply_compile_flags(simulator_correctness)

add_test(NAME simulator_correctness COMMAND simulator_correctness)
```

The compile definitions are **identical** to `simulator_smoke`'s. The PRIVATE
include of `${PROJECT_SOURCE_DIR}/src` is **identical**. The compile-flags
helper is **identical** — `gaby_vm_apply_compile_flags`, *not* the imported
relaxation helper, because the test is project-authored code held to the
project's normal warning policy.

**Why.**
- Same privileges as `simulator_smoke`, same scoping, same defines: nothing
  about this test's relationship with the imported headers differs from
  `simulator_smoke`'s.
- A future re-sync that needs to update VIXL build defines would change one
  block, then mechanically apply the same change to this block — the
  pattern is recognizable and grep-friendly.
- Reusing `gaby_vm_apply_compile_flags` (rather than a special helper)
  signals that the *test* code is held to project policy; the helper is the
  same one used by the project's own future code outside `src/`.

**Alternatives considered.**
- Factor a CMake helper `gaby_vm_add_simulator_test(name source)` that wraps
  the boilerplate. Rejected: two call sites do not justify an abstraction;
  a third one (this change) makes it tempting, but premature — the right
  trigger would be "we have five and the boilerplate is hiding intent",
  not three.

### D11. Simulator output: stdout via the constructor's default, no trace tweaking

**Decision.** The `Simulator` is constructed with the default output stream
(`stdout`), the same as `simulator_smoke`. No calls to set trace parameters
are made; the simulator's default trace level is what every sub-test runs
under, and CTest captures stdout for failure investigation.

**Why.**
- Matches `simulator_smoke` exactly. No new "what's the trace setting?"
  question for readers.
- CTest captures stdout per-test and only prints it on failure (or with
  `-V`), so default verbosity is harmless on passing runs and useful on
  failing ones.
- Trace tweaking (e.g., `set_trace_parameters(LOG_NONE)`) is an optimization
  for noisy tests; this test is small enough that we don't yet know whether
  it's noisy. Defer until it actually is.

**Alternatives considered.**
- Pass a temp-file or `/dev/null` stream to silence the simulator. Rejected
  per the previous bullet — premature.
- Force a quiet trace mode via `set_trace_parameters`. Same.

## Risks / Trade-offs

- **[Encoding transcription error]** — A hand-built `uint32_t` literal
  silently encodes a different instruction than its mnemonic comment claims.
  → **Mitigation**: D2's external-assembler verification at authorship time
  + mnemonic-per-line comment + test failure on wrong post-state acting as a
  belt-and-braces check during CI. Re-sync diffs surface drift naturally
  because the literals don't change unless someone edited them.

- **[Initial-register-state assumption]** — `Simulator::ResetRegisters` sets
  GPRs to `0xbadbeef`, not zero. A sub-test that reads a register it didn't
  write will see `0xbadbeef`, which can be mistaken for a passing assertion
  if the expected value also happens to be `0xbadbeef`. → **Mitigation**:
  every sub-test explicitly initializes every register it reads, even when
  zero would suffice; reviewers and the assertion API name (`expect_eq_*`)
  make the intent visible. Treat any sub-test relying on the reset value as
  a code-review red flag.

- **[VIXL ABI churn during re-sync]** — A future upstream change renames
  `WritePc` / `WriteXRegister` / `RunFrom` or alters their signatures.
  → **Mitigation**: the API surface is small (4–5 call sites) and is
  centralised in one TU. Re-sync triage updates this file alongside any
  imported-source changes; `git grep` finds every call instantly. The
  templated `RunFrom<R, Args...>` we're avoiding is exactly the kind of
  surface most likely to drift, so D4 helps here.

- **[Auditor blocks legitimate instructions]** — As in `simulator_smoke`, the
  CPU-features auditor's assertion inside `ExecuteInstruction` will reject
  any instruction whose feature isn't enabled. → **Mitigation**: every
  sub-test calls `sim.SetCPUFeatures(CPUFeatures::All())` immediately after
  Simulator construction, mirroring the smoke test. Encapsulate this in a
  small `make_simulator()` helper inside the TU so the call cannot be
  forgotten.

- **[Endianness / structure layout in load/store buffers]** — Tests that
  store a 64-bit value through a 64-bit address mode and then read back
  individual bytes need to know the simulator's memory model. → **Mitigation**:
  VIXL's simulator dereferences the register-held address directly into host
  memory, and the host process is little-endian on every supported platform
  (x86_64 macOS dev hosts, arm64 macOS / iOS targets). Cross-host portability
  is asserted at the build level; explicit byte-pattern fixtures in
  load/store sub-tests document the expected layout.

- **[Stack-state reuse across sub-tests]** — `SimStack` allocates a stack on
  Simulator construction. Per D5 we construct a fresh Simulator per sub-test,
  so this is a non-issue, but it's the kind of cross-test bleed that a
  "shared Simulator" alternative would have surfaced. Decision pinned in D5
  partly to prevent this.

- **[Test runtime growth]** — As coverage expands (SIMD, FP, more integer
  encodings), the linear "one TU, one CTest case" structure may stop being
  ergonomic. → **Mitigation**: the threshold for splitting into per-family
  binaries (or adopting a framework) is "the file no longer fits in a
  single reading session" — an explicit follow-up trigger, not a now-problem.

## Migration Plan

This change introduces a new test executable and adds a new requirement to the
`aarch64-simulator` spec. There is nothing to migrate: no existing test is
modified or removed, no source file in `src/` is touched, no public API
changes. Rollback is `git revert` of the single change commit; CTest will go
back to running two cases instead of three.

## Open Questions

1. **External assembler used for encoding verification (D2)** — The natural
   candidates are `clang -target aarch64-none-linux-gnu -c -` (or `arm64-darwin`
   for native macOS), `llvm-mc`, or upstream VIXL's own assembler at `../vixl`.
   All are acceptable; the file's top comment block should name whichever the
   author actually used so re-syncs can reproduce the verification. Pinned in
   `tasks.md`.

2. **Number of integer-encoding variants per mnemonic** — For `ADD`, do we
   exercise only `ADD Xd, Xn, Xm` (register form) or also `ADD Xd, Xn, #imm12`
   (immediate form), `ADD Xd, Xn, Xm, LSL #shift` (shifted form), and
   `ADD Wd, Wn, Wm` (32-bit form)? V1 minimum is one form per mnemonic;
   adding a second form per mnemonic doubles authorship cost for material
   coverage gain. Settle in `tasks.md`.

3. **Step-mode debug helper** — D4 mentions inspecting pre-`RET` state via
   single-stepping. We can instrument that via `ExecuteInstruction` in a
   loop with `IsSimulationFinished` checks. Useful for debugging future
   failures but not required for V1; flagged here so a future contributor
   doesn't reinvent it.

4. **Whether to assert NZCV directly** — `ReadNzcv()` exists on the simulator.
   For the `B.cond` test, asserting on NZCV after the `SUBS` (in addition to
   the marker-register post-state) would give a sharper failure mode if the
   condition decoding is wrong but the flag-setting is right. Lean toward
   yes; settle in `tasks.md` once we see the assertion-API call sites.
