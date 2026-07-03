# Fast-Dispatch Architecture — Adversarial Review and Synthesis, 2026-07-03

Provenance: multi-agent design round (4 constraint readings, 2 competing
designs, 1 adversarial review+synthesis) toward the goal 'applogic within
20-50x of native without JIT'. This file is the review+synthesis document;
it supersedes the two competing designs it reviews. The ordered OpenSpec
changes C1-C7 defined here are the execution plan.


---

# Adversarial Review — gaby-vm Fast-Execution Designs A & B

## 1. Correctness holes (vs. the leaf-state-contract checklist)

### H1 — CRITICAL, both designs: the two-representation story is incoherent as written

Both designs claim "one handler definition, two harnesses" selected by `#if GABY_THREADED`. But in a `GABY_THREADED=1` build **every handler body ends in a `musttail` dispatch** — calling any handler from `StepOnce` runs the *entire chain*, not one instruction. ShadowRunner (verified, `shadow_runner.cc:108-118`) calls `fast_.StepOnce()` per instruction **on the same production binary**. Neither design specifies how one binary hosts both a chaining and a single-stepping rep. The compile-time `#if` cannot do it. This breaks the single hardest external contract (leaf-state-contract §4/§5.12) at the design level.

**Fix (must be in the threading change):** a runtime step-mode gate inside the dispatch macro —

```cpp
#define GABY_DISPATCH(next)                                        \
  do {                                                             \
    if (GABY_UNLIKELY(sim->gaby_step_mode_)) return (next);        \
    GABY_MUSTTAIL return AsHandler((next)->handler)(sim, (next));  \
  } while (0)
```

`musttail` applies per return statement, so a guarded early return is legal. Cost: one L1 load + one always-false predicted branch per dispatch (near zero on Firestorm, and it makes ShadowRunner exercise the *same compiled handler bodies* as production). `gaby_step_mode_` is harness state like `trace_parameters_`: `ExecutionScope` must save/set/restore it (a nested threaded `RunFrom` inside an outer `StepOnce` must clear it), and it must **not** join the interpreter cursor. The alternative (dual template instantiation + a chain→step handler mapping) breaks once `form_hash` is repurposed, because nothing in the entry identifies the form to index a parallel step table. Take the runtime gate.

### H2 — CRITICAL, both: null PC after a hook must route to the terminal, not `FindEntry`

Hook returns 0 → `WritePc(0)` → `pc_ == nullptr` (verified h:1837-1862; today `StepOnce` h:1793 catches it *before* any range lookup). A branch handler (or generic thunk on its `pc_modified_` path) that unconditionally does `next = FindEntry(pc_)` turns clean termination into `GabyAbortPcNotInRange`. Neither design writes the null check into the handler contract. Required: `if (pc_ == nullptr) → dispatch terminal / return nullptr` on every WritePc-derived continuation.

### H3 — CRITICAL, both: range-end fallthrough is unhandled

Today, straight-line execution that walks off a range's end re-enters the range check, `FindRange` finds an *adjacently registered* range, and execution continues (verified h:1690-1702); abort only if no range covers the PC. Both designs delete the per-instruction range check and dispatch `e + 1` — **the last entry of a range tail-calls one past the end of `entries[]`**. Neither design mentions it. Fix: predecode appends one **boundary sentinel entry** per range (entries array = `size/4 + 1`) whose handler re-resolves `pc_` via `FindRange` (preserving cross-range fallthrough) or aborts. This touches the public `CodeRange` comment ("Array of size_bytes/4") and needs one sentence in the `pc:42-49` delta — an unstated spec touch in both designs.

### H4 — HIGH, both (Phase C/P4): NZCV-in-register sync is under-specified

Both mention spilling NZCV around hook calls and a stepped-rep adapter. Missing: **every specialized↔generic boundary**. Generic thunks call imported leaves that read/write the `nzcv_` member directly (`ConditionPassed`, `AddSubHelper`); with NZCV in x2, every generic-thunk prologue must spill x2→member and epilogue reload member→x2, and every MemWrite-observer re-entry inside a specialized store handler is another sync point. FPSR/FPCR stay members regardless (oracle compares them per step). This tax materially erodes P4's claimed win and reinforces keeping it measurement-gated.

### H5 — MEDIUM, Design A: BType and MOVPRFX epilogues cannot "emit nothing"

- A specialized ADD executing as a BLR target must still consume `next_btype_ → btype_ → Default`, or ShadowRunner's per-step BType compare diverges one instruction later. The gate (`btype_ || next_btype_ != Default`, h:1777) must be present in **every** handler epilogue, including specialized ones. Design A's "steady-state scalar handlers emit nothing" is only correct if it means "gate present, not taken." Design B states this correctly.
- MOVPRFX: specialized handlers must store `gaby_prev_was_movprfx_ = false` (Design B has this; A is vague), and should keep the one-load `if (unlikely(latch)) post-check` — otherwise an illegal MOVPRFX→scalar stream aborts on the debug track but sails through the cache track, violating `pc:161-165` track equality on the abort behavior.
- Repurposing `flags` bit0/bit1 in the 62-bit end state needs the rule neither design states: **entries with `flags&1` (BTI-relevant) or `flags&2` (MOVPRFX) are never specialized — they take the generic thunk.** Zero cost (BTI/MOVPRFX are absent from the business kernels), removes an entire correctness surface.

### H6 — MEDIUM, Design A: hand-inlined leaf math is a leaf rewrite in disguise

Design A's example re-derives add/flags math (`GabyAddWithCarry(..., sub ? ~op2 : op2, ...)`) and Phase D re-implements FP converts. This is exactly the `agents` "do not rewrite leaf semantics" hazard — FPSR cumulative flags, FPCR rounding/FZ/DN, and NaN propagation fail in input-dependent ways the TEST bodies may not fully cover. Binding rule for the synthesis: **specialized handlers CALL the existing non-virtual imported helpers** (`AddWithCarry<T>`, `ShiftOperand`, `FPAdd`/`FPMul`/`FPToFixed`...), never re-derive. Implement handlers as `static` member functions of `vixl::aarch64::Simulator` inside gaby marker blocks — free-function ABI for musttail, direct access to protected helpers, no friend gymnastics.

### H7 — Design B's 9th cursor field is incoherent; Design A's re-entry model is correct

Verified: `GabySaveCursor` snapshots **members only** (h:1922-1942). The threaded continuation `e` is a handler-frame *local argument* — there is nothing to snapshot; Design B's "add `e` to the cursor" delta cannot be implemented as written. Design A's model is right and strictly better: the hook is an ordinary (non-tail) call from a live frame; `e` survives on the frame; after the nested run the handler re-derives `next` from `pc_` — which also picks up `AddressUntag` for free on indirect targets. Adopt A; the `pc:256-268` delta becomes a clarifying scenario ("continuation is frame-local; `pc_` is authoritative at hook boundaries"), not a cursor-set change. Caveat that makes it sound: *every* handler that can experience re-entry (branch/hook, store/observer, thunk/leaf-callback) must derive its continuation from members **after** the re-entrant call — write this into the handler contract.

### H8 — LOW, both: keep BR/BLR/RET on the generic thunk

PAC auth, GCS push/compare, `WriteNextBType`, and the T5 interception gate (cc:4008-4124) make register-indirect branches the most contract-dense leaves. Both designs implicitly exclude them from specialization; make it explicit and permanent until a profile demands otherwise.

## 2. Unstated spec conflicts

1. **`pc:322-339` mix-independence vs. two handler classes (both designs miss it).** Verified text: "per-instruction non-leaf overhead SHALL therefore be independent of the workload's instruction mix." Specialized handlers (no form_hash store, no trace gate) vs. generic thunks (full epilogue) have *different* fixed overhead per form ⇒ overhead depends on the mix. The permitted-dynamic-work paragraph covers dynamic-state work, not differing fixed overhead. Needs a one-clause amendment in the specialization change: "SHALL NOT repeat encoding-derived work per execution; fixed non-leaf overhead MAY differ by predecode-assigned handler class."
2. **`pc:368-380` is touched at devirt time, not just at operand time.** The scenario freezes the three fields, and the header comment defines `leaf` as "opaque handle to the resolved VIXL leaf dispatcher." Storing a gaby handler fn-ptr keeps the type/offset but changes the documented meaning — fold a one-line amendment into the *first* change rather than deferring to Phase B/P3. Design A's end-state `{handler, operands}` field reorder additionally contradicts its own Phase B delta text; keep Design B's field-position-preserving layout.
3. **`pc:42-49` / `CodeRange` doc** — the boundary sentinel (H3) changes "array of size_bytes/4" to "+1"; needs a sentence in the same delta.
4. **Good news neither design cited:** the sim spec (verified ~line 163) says trace/debugger config "SHALL be ignored by the cache track" — hardcoding trace-off in cache-track handlers is spec-*aligned*, no delta needed.

## 3. Performance arithmetic

**Ground truth verified:** baseline M1 Pro parse 8.455 / hash 7.177 / struct 9.384 / fsm 8.944 / applogic 10.416 (numbers.md T5). The on-record profile (2026-06-11, **pre**-T1–T5, parse/fsm only) shows dispatch hub 38.6-42.1%, and its own estimate for full flatten+inline is **1.3-1.5×, "this step alone will not reach 50x."** The "ExecuteInstructionCached 23.6% self-time, fresh applogic profile" figure both designs build every prediction on **is not in the repo** — it must be re-measured before any prediction is committed (numbers.md itself says "re-profile first").

- **Same levers, contradictory claims:** Design A prices devirt+thread+range-hoist at −11% (→9.2); Design B prices the identical set at −26% (→7.7). Truth ≈ −15-20% (→ **8.4-8.9**): the thunk phase retains the entry load, pc_ store, BType/MOVPRFX gates, form_hash store, and adds the H1 step-mode load. B overclaims, A sandbags (making its later phases look better).
- **Design B P3 (7.7→3.4) is the largest single overclaim.** The eliminable-static budget is ~28% of *original* baseline (~2.9 ns), partially overlapping P1/P2's take. 7.7 − ~2.6 ≈ 5.1; call-boundary dissolution via inlining (real, unquantified) adds maybe 0.8-1.5 → **~3.8-4.5 realistic**. Design A's Phase C (−30-40% from register-residency alone) is its mirror-image overclaim; realistic ≈ −10-20% (see H4 sync tax and §4).
- **Both reach "~2.5" only by stacking their overclaims.** Honest landing zone for devirt+thread+specialize+operands+register-residency, no FP work, no fusion: **applogic ~3.3-4.5 ns; scalar shapes ~2.0-3.0 ns**. applogic's gap to 2.5 is dominated by FP/NEON (22.7% of its static mix; fcvts/FPIntegerConvert/LaneCountFromFormat named, more hidden in the ~25% unattributed tail). **Conclusion the designs dodge: FP-form specialization is not stretch work — it is on the committed-target critical path. Only fusion is genuinely optional.**
- **Physical floor check for 1.0 ns:** 3.2 cycles/insn @3.228 GHz. Even a lean specialized handler is ~10-14 µops with a per-insn indirect branch (~5% mispredict × ~13 cycles ≈ +0.65 c), a pc_ store, guest-register store-load traffic, and the H1 gate load. Practical floor ≈ 4-6 cycles ≈ **1.3-1.9 ns for the leanest scalar shapes**; applogic sits above that on FP latency. 1.0 ns is out of reach for this execution model.
- **L1-resident bias:** correctly self-diagnosed by the operand model (793× reuse ⇒ the benchmark cannot distinguish layouts; B+D chosen for deployment, not benchmark). Residual overclaim: "streamed 17.6 B/insn < 20" assumes all forms specialized; generic-thunk forms still re-read the guest word, so real streamed traffic is mix-dependent between 17.6 and 20+.

## 4. Musttail portability

- **Both designs' `preserve_none` gate is wrong on the primary platform.** `__clang_major__ >= 19` is meaningless on Apple Clang (Apple's own versioning; Xcode 16.x ≈ LLVM 17-era). Worse, **AArch64 `preserve_none` support landed ~upstream LLVM 19 and is not in shipping Apple/Xcode clang** — the flagship Phase-C/P4 lever is likely unavailable on iOS today. Gate purely on `__has_attribute(preserve_none)` + arch, and **demote preserve_none to opportunistic upside (Android NDK r27+/Linux clang≥19), not the step that reaches the committed number.**
- `[[clang::musttail]]` itself: fine on Apple clang (Xcode ≥13.3), guaranteed at `-O0` — but the `-O0` threaded `vixl_port` run must be an actual CI job, and the `GABY_THREADED=0` loop fallback must be a CI build too (gcc<15/HarmonyOS/sanitizer future). Neither design commits the CI matrix as tasks; make it explicit.
- Exceptions ON (verified: no `-fno-exceptions` anywhere): legal as long as no try scope / non-trivial-dtor local spans the tail site; the existing `GabyAbortPcNotInRange` cold-`noinline`-in-`.cc` pattern (verified h:1806-1810) is the enforced template.
- The H1 runtime-gate dispatch (two returns, musttail on one) is legal in both clang and gcc≥15.

## 5. Implementation-order risk

- **Design A bundles too much in its first change** (devirt+threading+range-hoist+terminal+two-rep in one landing). Design B's standalone devirt P1 is the right first move on a guard-rail-driven codebase: it fixes the handler ABI before ~15 specialized handlers are mass-produced, and lands the riskiest correctness deltas (H1/H2/H3) while *every form still executes byte-identical imported leaves* — maximally oracle-friendly.
- Devirt → thread → specialize is the right order (both agree modulo bundling); specializing first would mean rewriting every handler when the threading ABI lands.
- Both present specialization as one change; it must be **waved** (hottest scalar forms → operand64 plane → LS → FP), each wave behind `vixl_port` + `bench_business --verify`.
- Both park FP specialization in the stretch phase while quoting applogic targets — inconsistent with their own budget arithmetic (§3). Pull FP forward, ahead of the register-residency gamble whose primary-platform lever may not exist.

---

# THE SYNTHESIS

## Recommended architecture (pieces and provenance)

| Piece | Take from | Detail |
|---|---|---|
| Entry layout | **B** | Keep `{uint32_t form_hash-or-operand; uint32_t flags; const void* handler;}` field positions; 16 B static_assert; lean operands in freed bits per-form once a form is specialized; **no NEON/SVE flatten ever scheduled** (B's rejection is right — no applogic payoff, `form_hash` stays live for all generic-thunk forms). |
| Fat operands | both (= memo's own prescription) | Index-parallel `operand64` plane on `CodeRange`, fat forms only (logical-imm bitmask, PC-rel/literal absolute targets); reject uniform 32 B and var-length. |
| Handler taxonomy | both | Specialized / generic-thunk / sentinel, one exact signature `const PredecodedEntry* (*)(Simulator*, const PredecodedEntry*)`, handlers as `static` members of `vixl::aarch64::Simulator` in marker blocks. |
| Leaf math rule | **B** (hardened) | Specialized handlers **call the imported non-virtual helpers**; never re-derive flag/FP math (kills A's H6 exposure). |
| Re-entry model | **A** | No new cursor field; continuation is frame-local; re-derive `next` from `pc_` after any re-entrant call; `pc:256-268` delta is a clarifying scenario only. |
| Two-rep mechanism | **new (this review)** | Runtime `gaby_step_mode_` gate in `GABY_DISPATCH` (H1); saved/restored by `ExecutionScope` beside `trace_parameters_`. |
| Termination & boundaries | **new** | Explicit null-PC→terminal protocol (H2); per-range boundary sentinel entry preserving cross-range fallthrough and the hard abort (H3). |
| Never-specialize set | **new** | BR/BLR/RET/PAC/GCS forms, `flags&1` (BTI), `flags&2` (MOVPRFX), DoPrintf/pseudo-ops → generic thunk permanently until profiled otherwise. |
| Epilogue protocol | **B** (made binding) | Every handler: BType gate + `prev_was_movprfx_ = false` (+ gated post-check); specialized handlers hardcode trace-off (spec-aligned). |
| Register residency | **B's gating, hardened** | NZCV-as-param and `preserve_none` land only on a measured A/B win; `preserve_none` gated on `__has_attribute` only and treated as non-iOS upside. |
| Fusion | **B's deferral + A's drafted deltas** | Not scheduled; A's `pc:331-333`/`pc:42-43`/`pc:225-234` delta drafts parked in the design doc as the contingency's pre-written spec surface. |
| Phasing skeleton | **B**, re-waved | Devirt → thread → specialize (waves) → **FP wave (promoted)** → register-residency (gated) → fusion (contingency). |

## Ordered OpenSpec changes

**C0 (task inside C1, not a change): re-profile applogic on branch tip** — the 23.6% hub figure is not on record; all predictions below are conditional on it.

1. **`cache-dispatch-devirt`** — Handler ABI (2-arg, return-next; loop consumes the return), macro-generated statically-bound thunks (`sim->Simulator::VisitXxx(pc_)`), sentinel handlers, `ResolvePredecodeHandler` beside the existing seam.
   *Deltas:* `pc:368-380` one-line amendment (`leaf` slot holds the resolved *handler*; size/offsets/types unchanged). Design-note supersessions: D8 thunk deferral (`design:155-157`), the no-hard-Nx stance (`design:76-77/310-311/469`, recorded once). Expected: applogic **~9.4-9.8**.
2. **`cache-threaded-runfrom`** — `GABY_MUSTTAIL`/`GABY_THREADED` macros + loop fallback, runtime step-mode gate (H1), terminal handler + null-PC protocol (H2), boundary sentinel + cross-range fallthrough (H3), straight-line `e+1` range-check hoist, branch handlers re-resolve + hard abort.
   *Deltas:* `pc:256-268` scenario (frame-local continuation, `pc_` authoritative at hook boundaries — **no cursor-set change**); `pc:225-234`/`pc:161-165` two-rep scenario (identical handler bodies; ShadowRunner steps the stepped rep; whole-run threaded-vs-decoder differential covers the harness); `pc:42-49` boundary-sentinel sentence. Design-note: supersede `design:467` direct-threading non-goal. *New tests:* whole-run threaded-vs-stepped-vs-decoder; hook-in-chain re-entry; branch-to-unregistered abort; `-O0` threaded `vixl_port` CI job + `GABY_THREADED=0` CI build. Expected: **~8.4-8.9**.
3. **`cache-specialized-handlers-1`** — 6-8 hottest scalar forms (AddSub-imm/shifted, Logical-shifted, CondSelect, CondBranch, CompareBranch, TestBranch, MoveWide), lean operands in freed per-form bits, epilogue protocol per H5, imported-helper-call rule, never-specialize set.
   *Deltas:* `sim:110-124` specialized-handler scenario (equivalence by oracle, imported leaf byte-identical and debug-reachable); `pc:322-339` mix-independence clarification (**the conflict both designs missed**); `pc:368-380` per-form slot repurposing. Design-note: supersede `design:465`/memo finding 4 with the B+D record + fresh M1 Pro L1D A/B. Expected: **~6.5-7.3**.
4. **`cache-specialized-handlers-2`** — `operand64` plane (logical-imm bitmask kills DecodeImmBitMask 5.9%; PC-rel targets kill GetImmPCOffsetTarget 1.6%), LS-single/LS-pair (register-*number* storage, in-handler `AddressModeHelper` writeback preserved). *Deltas:* none new (covered by C3's). Expected: **~5.0-5.8**.
5. **`cache-fp-specialize`** *(promoted from stretch — on the committed-target critical path)* — applogic's scalar-FP/convert forms via imported FP helpers; `LaneCountFromFormat` hoisted to predecode. *Deltas:* none new. Expected: applogic **~3.6-4.4**; scalar shapes ~2.2-3.0.
6. **`cache-register-resident`** *(measurement-gated; explicit veto path)* — NZCV param with the full boundary-sync contract (H4); `preserve_none` where `__has_attribute` says so (expected: not iOS). *Deltas:* none normative; design-note records the A/B and the sync contract. Expected: applogic **~3.0-3.9** (better on Android/Linux than iOS).
7. **`cache-superinstruction-fusion`** *(contingency only, branch-dense shapes first)* — take Design A's pre-drafted `pc:331-333` carve-out, `pc:42-43` continuation-sentinel, `pc:225-234` per-committed-instruction amendments verbatim.

## First change (`cache-dispatch-devirt`) task-level scope

1. Re-profile applogic at branch tip (`sample`/Instruments, Release); record hub share in the change's numbers.md; abort/re-price if the hub is materially below ~20%.
2. Internal header (src-side, not public): `GabyHandler` typedef, `GABY_HANDLER_CC` placeholder, handler-contract comment block (trivially-destructible locals; RAII/hook work before dispatch; null-PC rule; epilogue protocol).
3. Macro-generate per-form generic thunks from `VISITOR_LIST_THAT_RETURN` as `static` Simulator members in a marker block: seat `form_hash_`, BTI gate, MOVPRFX latch protocol, statically-bound leaf call, post-check, `last_instr_`/IncrementPc equivalent, trace/BType gates — byte-equivalent to today's hub sequence (h:1684-1777).
4. `ResolvePredecodeHandler(form_hash)` beside `ResolvePredecodeLeaf`; populate pass stores handler fn-ptrs; sentinel handlers replace sentinel pmfs (`predecode_cache.cc:34-72`).
5. Rewire `ExecuteInstructionCached` to entry-lookup + one handler call; `StepOnce` unchanged externally; decoder track untouched.
6. Public-header comment touch-up for `leaf`→handler wording; `pc:368-380` delta; design-note supersessions.
7. Gates: `ctest -R vixl_port` (debug `-O0`), full debug ctest, `bench_business --verify`, 3-run before/after per shape; revert lever = flip populate pass back to pmf storage.

## Honest probability assessment

Conditional on the re-profile confirming ~20-25% hub share:

| Target | Probability | Basis |
|---|---|---|
| **applogic ≤ 2.5 ns** with C1-C6 | **~30-40%** | Both designs reach 2.5 only by stacking overclaims (B's P3 jump, A's Phase-C register-residency) on top of an unverifiable profile figure; honest landing zone is 3.0-4.0 ns; the plan's preserve_none lever is likely absent on iOS. |
| **applogic ≤ 2.5 ns** with C7 contingency (fusion + deeper FP/NEON work) | **~50-55%** | Fusion on branch-dense pairs + full FP-form coverage attacks what's left, at the heaviest spec cost (deltas pre-drafted). |
| **applogic ≤ 1.0 ns** | **≤5%** | 3.2 cycles/insn is below the practical floor (~4-6 cycles) of per-instruction, full-fidelity, oracle-steppable interpretation on this hardware; not reachable without abandoning the execution model (contradicting the no-JIT/no-IR constraints). |
| **scalar shapes ≤ 2.5 ns** | **~70-80%** | No FP floor; hash already at 7.18; lean handlers are the model's best case. |
| **scalar shapes ≤ 1.0 ns** | **~10-15%** | Only the leanest shapes, only if C6 lands well, likely only off-iOS. |

**Recommendation to record in the change's design.md:** commit 2.5 ns as the applogic acceptance bar with C7 explicitly named as its contingency; treat 1.0 ns as a scalar-shape aspiration, not an applogic commitment — no interpreter architecture that satisfies this repo's correctness contracts (per-instruction `StepOnce`, per-step full-state oracle, no fusion by default, no codegen) plausibly delivers 3.2 cycles per guest instruction on applogic.