## Why

Two project-level conventions need tightening, both about how source is written
rather than what it does:

(a) The marker convention for imported files currently writes the reason text
*on the same line* as the `// gaby-vm:` / `// gaby-vm BEGIN:` token, and aligns
any continuation lines to the column right after `gaby-vm:`. That alignment is
expensive to maintain: adding a single word reflows the whole aligned block,
and the deep indent wastes horizontal room — worse with CJK reason text, which
is what the existing markers use.

(b) There is no written rule about braces on single-statement control flow in
project-authored code, so the style drifts file to file.

## What Changes

- **Marker style (imported files).** The `gaby-vm` marker *token* now occupies
  its own line; the reason starts on the following line(s) as ordinary `//`
  comments — no special alignment. Applies to both `// gaby-vm:` (single-line
  edits) and `// gaby-vm BEGIN:` (multi-line edits). `// gaby-vm END` is a
  token-only line already and is unchanged.
  - **Core contract preserved (not BREAKING):** the marker token stays
    lowercase `gaby-vm`; `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` still
    enumerates every drift (one hit per marker start); deleted upstream code is
    still left commented out inside `BEGIN/END`. Marker *semantics* and the
    reason *text* are not changed — only how the comment is laid out.
- **Rewrite existing markers.** Every marker region under `src/` is rewritten
  to the new layout.
- **Spec + docs updated** to describe the new layout. Semantics unchanged.
- **New brace convention.** In project-authored code (`src/gaby_vm/`,
  `include/gaby_vm/`, `test/`), every single-statement body of
  `if` / `else` / `for` / `while` / `do-while` must use braces and a line
  break — no one-line bodies. Imported VIXL files are **exempt**: byte-identity
  with upstream wins, so their existing style is left alone.
- **`.clang-format` configured** for the machine-enforceable part of the brace
  rule (e.g. `InsertBraces`, short-statement-on-one-line disabled); the
  remainder is documented in `conventions.md` and left to review.
- **Fix existing violations** of the brace rule in `src/gaby_vm/` and `test/`.

## Capabilities

### New Capabilities

(none — the brace convention is a `docs/conventions.md` + `.clang-format`
change, not a normative capability spec.)

### Modified Capabilities

- `aarch64-simulator`: the marker-convention requirements are reworded to the
  token-on-its-own-line layout. Affected: *All edits to imported files use the
  documented marker convention* (the requirement text plus the *Single-line
  edit* and *Multi-line edit* scenarios), and the marker-region wording in
  *Imported files are byte-identical to upstream except at marked locations*
  (the *Modified file matches upstream outside marker regions* scenario).
  Behavior and the grep-enumeration guarantee are unchanged.

## Impact

- **Spec:** `openspec/specs/aarch64-simulator/spec.md` — marker requirement and
  scenario wording.
- **Docs:** `docs/conventions.md` (rewrite the *Marker convention for imported
  files* section; add the new brace rule) and `docs/architecture.md` (the
  marker-layout description in *VIXL import boundary*).
- **Imported source:** every marker region under `src/aarch64/` — 1 single-line
  `// gaby-vm:` marker and 10 `BEGIN/END` blocks across `instructions-aarch64.cc`
  and `simulator-aarch64.h`. Note: `git grep` also matches a line in
  `src/.clang-format` that *describes* the convention in prose — that is not a
  marker and needs no change.
- **Project code:** brace-rule cleanup in `src/gaby_vm/` and `test/`.
- **Config:** root `.clang-format`; likely a new `src/gaby_vm/.clang-format`,
  because `src/.clang-format` currently sets `DisableFormat: true` and
  clang-format's directory walk would otherwise apply that to `src/gaby_vm/`.
- **`AGENTS.md`** (the `CLAUDE.md` symlink): currently has no inline marker
  examples; verify during implementation and touch up only if wording drifts.
