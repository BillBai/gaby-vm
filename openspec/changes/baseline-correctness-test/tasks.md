## 1. Resolve design open questions (pin choices before any code)

- [x] 1.1 **External assembler (design Open Q1)**: use the **clang+otool** pair shipped with Xcode CommandLineTools for encoding verification at authorship time. Exact invocation: `printf '<asm>\n' | clang -target arm64-apple-darwin -c -x assembler -o /tmp/enc.o - && otool -t /tmp/enc.o` — the second column of `otool -t`'s output is the 32-bit instruction word in the same hex form we write into the C++ source. (Note: the original plan named `llvm-mc -triple=aarch64 -show-encoding`, but `llvm-mc` is not bundled with Xcode 26.4 on this dev machine, while `clang` and `otool` are guaranteed parts of every macOS dev install — strictly more portable.) Record the invocation in the top-of-file comment block (task 3.5).
- [x] 1.2 **Encoding-variant count per mnemonic (design Open Q2)**: V1 covers **one canonical form per mnemonic** — the register-register form for arithmetic/logical (`<op> Xd, Xn, Xm`), 64-bit `LDR`/`STR` with the two required addressing modes (immediate offset and register offset), and the standard encodings for `B.cond`/`CBZ`/`CBNZ`/`BL`/`RET`. Adding immediate, shifted, or 32-bit-W variants is a follow-up change once an integer-correctness gap actually surfaces.
- [x] 1.3 **Step-mode debug helper (design Open Q3)**: **defer**. If a future failure needs single-stepping, add a `step_until_finished(Simulator&)` helper at that point. Do not pre-build it.
- [x] 1.4 **NZCV-direct assertion on `B.cond` (design Open Q4)**: **yes** — the `B.cond` sub-tests SHALL assert directly on `sim.ReadNzcv()` after `RunFrom` in addition to the marker-register assertion. This sharpens the failure diagnostic when a `B.cond` regression hits: a marker-only failure cannot distinguish "wrong flag setter" from "wrong condition decode", and asserting NZCV separates the two.

## 2. CMake wiring against an empty source (isolate wiring failures first)

- [x] 2.1 Create `test/simulator_correctness.cc` with the BSD-3-Clause license header (matching `simulator_smoke.cc`), an empty `int main() { return 0; }`, and a placeholder TODO comment naming the change ID (`baseline-correctness-test`).
- [x] 2.2 Append a new block to `test/CMakeLists.txt` immediately after the existing `simulator_smoke` block, structurally identical (per design D10): `add_executable(simulator_correctness simulator_correctness.cc)`, `target_link_libraries(simulator_correctness PRIVATE gaby_vm::gaby_vm)`, `target_include_directories(simulator_correctness PRIVATE ${PROJECT_SOURCE_DIR}/src)`, `target_compile_definitions(simulator_correctness PRIVATE VIXL_INCLUDE_TARGET_A64 VIXL_INCLUDE_SIMULATOR_AARCH64 $<$<CONFIG:Debug>:VIXL_DEBUG>)`, `gaby_vm_apply_compile_flags(simulator_correctness)`, `add_test(NAME simulator_correctness COMMAND simulator_correctness)`. Include a one-line comment pointing to `specs/aarch64-simulator/spec.md` and `design.md` (matching the simulator_smoke block's pattern).
- [x] 2.3 Configure and build both presets: `cmake --preset dev-debug && cmake --build build/debug` and `cmake --preset dev-release && cmake --build build/release`. Confirm both succeed with no new warnings.
- [x] 2.4 Confirm CTest enumeration: `ctest -N --preset dev-debug` lists `simulator_correctness` alongside `smoke` and `simulator_smoke`.
- [x] 2.5 Confirm the empty test runs green: `ctest --preset dev-debug -R '^simulator_correctness$'` (and for release: `ctest --test-dir build/release -R '^simulator_correctness$'`, since the project only declares a `dev-debug` test preset).

## 3. Harness scaffolding (assertion API + factory + family stubs)

- [x] 3.1 Add includes: `<array>`, `<cstdint>`, `<cstdio>`, `<cstring>`, `cpu-features.h`, `aarch64/decoder-aarch64.h`, `aarch64/simulator-aarch64.h` (mirroring `simulator_smoke.cc`'s include order).
- [x] 3.2 In an anonymous namespace, define a `TestState` struct: `int passed = 0; int total = 0; const char* current_subtest = ""; const char* current_family = "";`.
- [x] 3.3 Define assertion helpers in the same anonymous namespace:
      a) `bool expect_eq_u64(TestState&, uint64_t actual, uint64_t expected, const char* label)`,
      b) `bool expect_eq_u32(TestState&, uint32_t actual, uint32_t expected, const char* label)`,
      c) `bool expect_eq_u8(TestState&, uint8_t actual, uint8_t expected, const char* label)`.
      Each increments `total`, increments `passed` only on equality, and on inequality `std::fprintf(stderr, ...)` a diagnostic naming the family, sub-test, label, and both hex-printed values; returns `true` on equality so callers can chain.
- [x] 3.4 Define a `make_simulator(Decoder& decoder)` factory (or a small RAII wrapper) that constructs a `Simulator`, immediately calls `sim.SetCPUFeatures(CPUFeatures::All())` (per design Risk: "Auditor blocks legitimate instructions"), and returns it. Use this in every sub-test so the auditor configuration cannot be forgotten. — Implemented as a free `configure_simulator(Simulator&)` helper that callers invoke right after `Simulator sim(&decoder, stdout);`; single point of decision for which CPU features to enable.
- [x] 3.5 Add a top-of-file comment block (15–25 lines) documenting (a) why no assembler is imported (Tier 0 boundary), (b) the exact clang+otool invocation used to verify each encoding (`printf '<asm>\n' | clang -target arm64-apple-darwin -c -x assembler -o /tmp/enc.o - && otool -t /tmp/enc.o`, where the second column of the output is the 32-bit instruction word as it appears in the C++ literal), (c) how to reproduce the verification during re-syncs, and (d) the termination protocol (`RET` to NULL `LR`, per design D3).
- [x] 3.6 Add family-function stubs: `run_arithmetic_tests(TestState&)`, `run_logical_tests(TestState&)`, `run_loadstore_tests(TestState&)`, `run_branch_tests(TestState&)`, `run_call_return_tests(TestState&)`. Each currently does nothing.
- [x] 3.7 Implement `main()` to:
      a) declare a local `TestState`,
      b) call each family in turn,
      c) `std::printf("simulator_correctness: %d/%d sub-tests passed\n", state.passed, state.total);`,
      d) return `state.passed == state.total ? 0 : 1`.
- [x] 3.8 Rebuild both presets and re-run the test; confirm output `simulator_correctness: 0/0 sub-tests passed` and exit 0. — Verified under `build/debug` (Passed, 0.68s); 4 `-Wunused-function` warnings on the helpers are expected at this scaffolding stage and resolve as Groups 4–8 wire in callers.

## 4. Integer arithmetic family (ADD, SUB, MUL)

- [x] 4.1 ADD: verify encoding `add x0, x1, x2` → `0x8b020020` with `clang+otool`; in `run_arithmetic_tests`, add a sub-test that writes a known X1 and X2, runs the 2-word sequence `[ADD; RET]`, and asserts `ReadXRegister(0) == X1 + X2` (host uint64_t addition) with `expect_eq_u64`.
- [x] 4.2 SUB: verify encoding `sub x0, x1, x2` (→ `0xcb020020`); add a sub-test on the same pattern, expected = `X1 - X2` (host uint64_t subtraction, wrapping arithmetic).
- [x] 4.3 MUL: verify encoding `mul x0, x1, x2` (→ `0x9b027c20`, MADD with Ra=XZR); add a sub-test, expected = `X1 * X2` (host uint64_t multiplication, low 64 bits).
- [x] 4.4 Pick input values that exercise carry / signed-vs-unsigned distinctions where cheap (e.g. for SUB: a case where `X1 < X2`, exercising wrap-around). Document the choice rationale in a brief comment above each sub-test. — ADD uses bit-complement pair summing to UINT64_MAX; SUB uses `7 - 10` for wrap; MUL uses 0x1_00000003 × 0x2_00000005 for >64-bit overflow.
- [x] 4.5 Build and run; confirm output `simulator_correctness: 3/3 sub-tests passed`. — Verified under both debug (0.82s) and release (0.40s); all 3 sub-tests pass.

## 5. Logical family (AND, ORR, EOR)

- [x] 5.1 AND: verify encoding `and x0, x1, x2` (register form, not the immediate form, → `0x8a020020`); add sub-test, expected = `X1 & X2`.
- [x] 5.2 ORR: verify encoding `orr x0, x1, x2` (→ `0xaa020020`); add sub-test, expected = `X1 | X2`.
- [x] 5.3 EOR: verify encoding `eor x0, x1, x2` (→ `0xca020020`); add sub-test, expected = `X1 ^ X2`.
- [x] 5.4 Choose input values that distinguish each mnemonic (e.g. operand pairs where `AND != OR != XOR`, so any encoding swap would surface as a wrong result, not as a coincidental match). — Using `a = 0xAA…AA`, `b = 0xCC…CC`: AND = `0x88…88`, ORR = `0xEE…EE`, EOR = `0x66…66`. All three results are distinct by every nibble.
- [x] 5.5 Build and run; confirm `5/6`-pattern progression, ending at `6/6 sub-tests passed` after both groups. — Verified: `simulator_correctness: 6/6 sub-tests passed` under both debug (0.30s) and release (0.73s).

## 6. Load/store family (LDR, STR — immediate and register offsets)

- [x] 6.1 Define a small helper buffer fixture inside `run_loadstore_tests`: `alignas(uint64_t) std::array<uint8_t, 64> buf; buf.fill(0);` (per design D6). — One fixture at function scope, prefilled/cleared per sub-test.
- [x] 6.2 LDR Xt, [Xn, #imm]: verify encoding `ldr x0, [x1, #16]` (→ `0xf9400820`, imm12=2 scaled by 8); prefill `buf[16..23]` with `0xCAFEBABEDEADBEEF`; write `&buf[0]` into X1; run `[LDR; RET]`; assert `ReadXRegister(0)` matches the prefilled pattern.
- [x] 6.3 LDR Xt, [Xn, Xm]: verify encoding `ldr x0, [x1, x2]` (→ `0xf8626820`, LSL #0); same buffer fixture, write `&buf[0]` to X1 and `16` to X2; assert.
- [x] 6.4 STR Xt, [Xn, #imm]: verify encoding `str x0, [x1, #24]` (→ `0xf9000c20`, imm12=3); clear buf, write `0xDEADBEEFFEEDFACE` to X0 and `&buf[0]` to X1; run `[STR; RET]`; read back via `memcpy` and assert.
- [x] 6.5 STR Xt, [Xn, Xm]: verify encoding `str x0, [x1, x2]` (→ `0xf8226820`); same approach with register-offset addressing; assert buffer state.
- [x] 6.6 Verify each encoding's bit pattern against an external `clang+otool` invocation; the immediate's scaling (×8 for 64-bit LDR/STR) is a common encoding pitfall — write the chosen immediate explicitly in the comment. — Top comment in `run_loadstore_tests` documents the scaling; encodings verified in one batch via clang+otool.
- [x] 6.7 Build and run; confirm `10/10 sub-tests passed`. — Verified: `simulator_correctness: 10/10 sub-tests passed` under both debug (1.14s) and release (0.46s).

## 7. Conditional control flow family (B.cond, CBZ, CBNZ — taken and not-taken each)

- [x] 7.1 Define a "marker write" pattern for branch tests: a 3-word sequence `[set X0 to sentinel A; <conditional branch over the next instruction>; set X0 to sentinel B; RET]`. Taken → X0 ends as A. Not-taken → X0 ends as B. Or equivalently use two markers in different registers to distinguish. — Implemented as: X0 = `sentinel_a (0xA0A0)` set via `WriteXRegister` before RunFrom; encoded sequence is `[CBZ/CBNZ/B.EQ +8; MOV X0, #0xB0B0; RET]`. Taken skips the MOV, X0 stays `sentinel_a`; not-taken executes MOV, X0 becomes `sentinel_b (0xB0B0)`.
- [x] 7.2 CBZ taken: verify encoding `cbz x1, #+8` (→ `0xb4000041`); set X0=sentinel_a via WriteXRegister, X1=0; assert X0 stays sentinel_a.
- [x] 7.3 CBZ not-taken: same encoding, X1=1; assert X0 becomes sentinel_b.
- [x] 7.4 CBNZ taken/not-taken: mirror of CBZ with X1 inverted (→ `0xb5000041`). Taken: X1=1; not-taken: X1=0.
- [x] 7.5 B.cond taken: prepend a `subs x3, x1, x1` (→ `0xeb010023`, forces Z=1); encode `b.eq #+8` (→ `0x54000040`); assert X0 stays sentinel_a AND `sim.ReadZ()` returns true (per task 1.4 — using the `bool Simulator::ReadZ() const` accessor at simulator-aarch64.h:2177).
- [x] 7.6 B.cond not-taken: prepend `subs x3, x1, x2` (→ `0xeb020023`) with X1=10, X2=3 (forces Z=0); same `b.eq` encoding; assert X0 becomes sentinel_b AND `sim.ReadZ()` returns false.
- [x] 7.7 Verify each encoding (`cbz`, `cbnz`, `b.eq`, `subs`) with `clang+otool` and document the immediate-offset arithmetic in a comment (PC-relative, scaled by 4 for these branch encodings). — All four verified in one batch via clang+otool; encoded as `imm19=2` (= +8 bytes / 4) inside each branch word.
- [x] 7.8 Build and run; confirm sub-test count. — Result: **18/18 sub-tests passed** under both debug (1.44s) and release (0.48s). Note: 18, not 16. The current `expect_eq_*` helpers count assertions, not scope blocks, and the two B.cond sub-tests each carry two assertions (marker register + NZCV.Z) per task 1.4. End-of-Group-7 total = 10 (prior) + 4 (CBZ/CBNZ × 1 assert each) + 4 (B.cond × 2 asserts each) = 18.

## 8. Procedure call/return family (BL + RET, with outer-LR preservation)

- [x] 8.1 Lay out the full sequence per design D7 — prologue+call+epilogue+outer-RET, then callee, then inner RET. Roughly:
      `[MOV X19, LR; BL callee; MOV LR, X19; RET; callee: MOV X0, #sentinel; RET]`. Note that `MOV Xd, Xn` on AArch64 is encoded as `ORR Xd, XZR, Xn`, and `MOV LR, X19` similarly; verify both with `clang+otool`. — `MOV X19, LR = 0xaa1e03f3 (orr x19, xzr, x30)` and `MOV LR, X19 = 0xaa1303fe (orr x30, xzr, x19)` verified.
- [x] 8.2 Verify each encoding with `clang+otool`. The `BL` immediate is PC-relative byte offset divided by 4 — compute it from the callee's word index minus the BL's word index; document the arithmetic in a comment so future edits don't drift the offset. — `BL callee` from PC=4 to callee at PC=16 → offset +12 → imm26 = 3 → encoding `0x94000003`. PC arithmetic table is in the source comment.
- [x] 8.3 Write the sentinel value to be something not normally produced by the prologue (e.g. `0xc011ab1e`) so a wrong-callee-body or wrong-return-target outcome surfaces clearly. — Using `0xC011AB1E`. Encoded in callee as `MOVZ X0, #0xAB1E (0xd29563c0)` + `MOVK X0, #0xC011, LSL #16 (0xf2b80220)`. The `LSL #16` was a hand-encoding pitfall the design's "Encoding transcription error" risk warned about — clang+otool caught the `hw` field bits.
- [x] 8.4 Sub-test asserts:
      a) `sim.ReadXRegister(0) == sentinel` (callee body ran AND control returned to the prologue's continuation, not somewhere else),
      b) the simulator actually terminated (we got back from `RunFrom`; if we didn't, the test process is hanging and CTest will time out). — One `expect_eq_u64` covers (a); (b) is implicit (test reaches the assertion line at all).
- [x] 8.5 Build and run; confirm sub-test count. — Result: **19/19 sub-tests passed** under both debug (1.54s) and release (0.52s). Total = 18 (post-Group-7) + 1 (BL+RET single assertion) = 19.

## 9. End-to-end verification against the spec

- [x] 9.1 **R-A.1 (CTest enumeration)**: `ctest -N --preset dev-debug` includes `simulator_correctness`. — Verified: Test #1 smoke, #2 simulator_smoke, #3 simulator_correctness.
- [x] 9.2 **R-A.2 (dev-debug passes)**: `ctest --preset dev-debug -R '^simulator_correctness$' -V` exits 0 and prints sub-test count. — Verified: `simulator_correctness: 19/19 sub-tests passed`, Passed in 1.30s.
- [x] 9.3 **R-A.3 (dev-release passes)**: same for `dev-release` (`ctest --test-dir build/release`). — Verified: `19/19 sub-tests passed`, Passed in 0.25s.
- [x] 9.4 **R-A.4 (build pattern matches `simulator_smoke`)**: visual diff of the two `add_executable` blocks in `test/CMakeLists.txt`; confirm the include path, the three VIXL defines, and the compile-flags helper line are identical. — Verified via `diff` of the two awk-extracted blocks: only the executable name, the comment block, and the test-name token differ; PRIVATE include path, the three VIXL defines, and `gaby_vm_apply_compile_flags` line are byte-identical.
- [x] 9.5 **R-A.5 (no `src/` edits)**: `git diff --name-only main..HEAD -- src/` returns empty. — Verified empty.
- [x] 9.6 **R-A.6 (no public-header edits)**: `git diff --name-only main..HEAD -- include/` returns empty. — Verified empty.
- [x] 9.7 **R-B.1 (arithmetic covered)**: grep test source for `// add `, `// sub `, `// mul ` mnemonics in encoding comments; confirm each appears at least once. — Verified: lines 162, 182, 204.
- [x] 9.8 **R-B.2 (logical covered)**: grep for `// and `, `// orr `, `// eor `; confirm each appears. — Verified: lines 237, 253, 269.
- [x] 9.9 **R-B.3 (load/store both modes)**: grep for `// ldr ` and `// str ` with both `[x?, #` and `[x?, x?` patterns; confirm at least one of each. — Verified: LDR [#] at 300, LDR [Xm] at 317, STR [#] at 337, STR [Xm] at 356.
- [x] 9.10 **R-B.4 (conditional branches both directions, B.cond has explicit flag-setter)**: grep for `// b.eq`, `// cbz`, `// cbnz`; confirm taken / not-taken pairing in adjacent sub-tests; confirm a `// subs ` (or `// cmp `) appears in the same sub-test as every `// b.eq ` (or other B.cond variant). — Verified: CBZ taken/not-taken at 387/406, CBNZ at 426/445, B.EQ at 471/496 each preceded by SUBS at 470/495.
- [x] 9.11 **R-B.5 (BL+RET, both call and return observable)**: grep for `// bl ` and confirm the matching sub-test reads a register written only inside the callee. — Verified: BL at 546; X0 is written only inside the callee (via MOVZ/MOVK at 549/550) and asserted post-RunFrom. The two RETs at 548 (outer) and 551 (inner) bracket the call.
- [x] 9.12 **R-B.6 (no assembler/emit helpers in source)**: `git grep -nE 'class [A-Z][A-Za-z]*Assembler|MacroAssembler|EmitInstruction' test/simulator_correctness.cc` returns empty. — Verified empty.
- [x] 9.13 **R-B.7 (every sequence ends with `RET`, no manual `WriteLr`)**: grep for `0xd65f03c0` (the `RET X30` encoding) — appears at least once per family; `git grep -n 'WriteLr' test/simulator_correctness.cc` returns empty. — Verified: 19 occurrences of `0xd65f03c0` (18 in encoded sequences across all sub-tests + 1 in the top comment); no `WriteLr` calls anywhere.
- [x] 9.14 **R-B.8 (failing sub-test produces a diagnostic)**: temporarily flip one expected value to a wrong one, rebuild, run, confirm stderr names the failing sub-test and shows actual vs. expected hex values; revert the flip. — Verified by temporarily changing the ADD expected value to `(a + b) ^ 1ULL`; diagnostic emitted `[FAIL] arithmetic / ADD x0, x1, x2: X0 = X1 + X2 (...)  actual = 0xffffffffffffffff (18446744073709551615)  expected = 0xfffffffffffffffe (18446744073709551614)`, summary dropped to `18/19 sub-tests passed`, CTest reported FAILED. Flip reverted; back to 19/19.
- [x] 9.15 Run `openspec validate baseline-correctness-test --strict` and confirm `valid: true`. — Verified: `valid: true, issues: []`.
- [x] 9.16 Run the project Stop hook's full build + tests under both presets one final time; confirm green. — Final full ctest run: 3/3 tests passed under both `build/debug` (total 0.93s) and `build/release` (total 0.10s).

## 10. Final touches

- [x] 10.1 Run clang-format on `test/simulator_correctness.cc` (the project's clang-format-on-edit hook should already keep it formatted; this is a belt-and-braces final check). The file is project-authored, so `src/.clang-format`'s `DisableFormat: true` does NOT apply. — Verified: `clang-format --dry-run --Werror test/simulator_correctness.cc` produced no output (clean).
- [x] 10.2 Review the top-of-file comment block one more time: does a reader who has never seen this change understand (a) why hand-encoded literals, (b) how encodings were verified, (c) how to add a new sub-test, (d) how termination works? Tighten any unclear sentence. — Re-read; all four points covered (Tier-0 boundary rationale, clang+otool invocation with literal copy-paste workflow, mnemonic-comment convention, RET/LR-NULL termination protocol including the BL+RET preservation note).
- [x] 10.3 Stage and commit: `git add test/simulator_correctness.cc test/CMakeLists.txt` (no other paths should be touched); commit message follows the project's conventional-commits style — `feat: add simulator_correctness baseline test for AArch64 leaves`. — Committed as `289fb2f`. Scope intentionally widened to also include `tasks.md` (completion marks, naturally part of the same logical change) and `overview.html` (the visualization the user requested mid-session); commit message lists what's covered and why.
- [ ] 10.4 Archive the change: `openspec archive baseline-correctness-test` (folds the spec delta into `openspec/specs/aarch64-simulator/spec.md` and moves the change folder to `openspec/changes/archive/`).
