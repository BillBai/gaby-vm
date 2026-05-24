## 1. clang-format configuration

- [x] 1.1 Add the brace keys to the root `.clang-format` — `InsertBraces: true`, `AllowShortIfStatementsOnASingleLine: Never`, `AllowShortLoopsOnASingleLine: false` — leaving the existing VIXL copyright header and all other keys untouched.
- [x] 1.2 Create `src/gaby_vm/.clang-format` with `BasedOnStyle: InheritParentConfig`, `DisableFormat: false`, `SortIncludes` restored (e.g. `CaseSensitive`), and the same three brace keys; give it an explanatory comment and no BSD header, mirroring `src/.clang-format`'s precedent for project-authored config files. Also add `BasedOnStyle: InheritParentConfig` to `src/.clang-format` so the inheritance chain reaches the root Google config — inert for imported files because `DisableFormat: true` means the base style is never applied (see design.md D4).
- [x] 1.3 Verify the topology with `clang-format --dump-config`: from a file inside `src/gaby_vm/` the merged config shows `DisableFormat: false` and `InsertBraces: true`; from a file inside `src/aarch64/` it still shows `DisableFormat: true`.

## 2. Rewrite imported-file markers to the new layout

- [x] 2.1 Rewrite the single-line `// gaby-vm:` marker in `src/aarch64/simulator-aarch64.h` to a standalone `// gaby-vm:` token line followed by ordinary `//` reason lines.
- [x] 2.2 Rewrite all 10 `// gaby-vm BEGIN:` blocks — 2 in `src/aarch64/instructions-aarch64.cc`, 8 in `src/aarch64/simulator-aarch64.h` — so each `BEGIN:` token line stands alone with the reason on ordinary `//` lines below it; leave every `// gaby-vm END` line unchanged.
- [x] 2.3 Preserve every marker's reason wording verbatim (only line breaks may change) and keep all `docs/refs/*.md` path references intact; do not introduce the literal sequences `gaby-vm:`, `gaby-vm BEGIN`, or `gaby-vm END` into any reason line. (Chinese-reason markers keep their original break points; only the 2 English-reason markers in `instructions-aarch64.cc` were re-wrapped, since wrapping English at spaces is lossless.)
- [x] 2.4 Verify enumeration: `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` still matches every marker, `BEGIN` count equals `END` count, and no token line carries trailing reason text. (The hit in `src/.clang-format` is descriptive prose, not a marker — it needs no change.)
- [x] 2.5 Verify byte-identity: `git diff` of `src/aarch64/` versus pre-change shows only marker comment lines changed — no bracketed upstream code moved.

## 3. Update documentation

- [x] 3.1 Rewrite the "Marker convention for imported files" section of `docs/conventions.md` to describe the token-on-its-own-line layout, with a short before/after example for both the single-line and `BEGIN/END` forms.
- [x] 3.2 Update the marker-layout description in the "VIXL import boundary" section of `docs/architecture.md` to match the new layout.
- [x] 3.3 Add the brace convention to `docs/conventions.md`: project code under `src/gaby_vm/`, `include/gaby_vm/`, and `test/` uses braces and a line break on every single-statement `if` / `else` / `for` / `while` / `do-while` body; imported VIXL files are exempt; note which part `clang-format` enforces and which part is left to review.
- [x] 3.4 Check `AGENTS.md` (the `CLAUDE.md` symlink) for marker examples or layout wording; update only if it shows the old inline layout. (AGENTS.md only links to the docs — no inline marker layout — so no change needed.)

## 4. Bring project code into brace-rule compliance

- [x] 4.1 Run `clang-format -i` over `src/gaby_vm/`, `include/gaby_vm/`, and `test/`; review the diff and keep only brace-related changes for tracked files. (`include/gaby_vm/` was already clean; `test/reentrancy_test.cc` + `test/workload_shadow_test.cc` had one brace fix each. The `src/gaby_vm/*.cc` files are new/untracked and were fully formatted under their new `.clang-format` — no committed baseline to churn, and a format-on-save hook reformats them on any edit regardless; see design.md non-goals/risks.)
- [x] 4.2 Manually fix any single-statement `if` / `else` / `for` / `while` / `do-while` violations that `InsertBraces` did not catch. (None — `clang-format --dry-run` reports every in-scope file clean, so `InsertBraces` caught all violations.)

## 5. Verify

- [x] 5.1 Configure and build under the `dev-debug` and `dev-release` presets; confirm both build green.
- [x] 5.2 Run the CTest suite under both presets; confirm all tests pass. (6/6 under each preset; `dev-release` has no `ctest` preset registered so it ran via `ctest` from `build/release/` directly.)
- [x] 5.3 Final audit: re-run the enumeration grep and the `src/` byte-identity diff to confirm the marker contracts still hold after all edits. (10 `BEGIN` ↔ 10 `END` balanced, 1 `// gaby-vm:`, every token line is bare; the only non-comment removed lines in `src/aarch64/` are pre-existing feature work *inside* marker regions, not from this rewrite.)
