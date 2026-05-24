## Context

Two project-level conventions are being tightened (see `proposal.md` for the
motivation). This document pins down the *how* — exact grammar, the
`clang-format` topology, and the migration order — because both conventions
touch imported files and the `.clang-format` setup is more subtle than it
looks.

Current state:

- **Markers.** The marker convention records drift from upstream inside
  imported files. Today the reason text sits on the same line as the
  `// gaby-vm:` / `// gaby-vm BEGIN:` token, and continuation lines are
  space-aligned to the column after `gaby-vm:`. There is 1 single-line marker
  and 10 `BEGIN/END` blocks across `src/aarch64/instructions-aarch64.cc` and
  `src/aarch64/simulator-aarch64.h`.
- **`.clang-format` topology.** clang-format resolves the *nearest ancestor*
  `.clang-format` for each file; configs do not merge unless a config opts in
  with `BasedOnStyle: InheritParentConfig`. The repo has two:
  - root `.clang-format` — Google-based, VIXL-derived (carries a VIXL
    copyright header); governs the repo root, `include/`, `test/`.
  - `src/.clang-format` — `DisableFormat: true`, `SortIncludes: Never`;
    governs *everything* under `src/`, which keeps imported sources
    byte-identical to upstream. Because the walk is directory-based, it also
    governs `src/gaby_vm/` — so project-authored code there is currently not
    formatted at all.
- **clang-format version** on this machine is 22.1.3, so `InsertBraces` (added
  in clang-format 17) and `BasedOnStyle: InheritParentConfig` are both
  available.

Constraint that shapes everything below: imported files under `src/` must stay
byte-identical to upstream *except inside marker regions*. The marker comment
lines themselves are part of the marker region, so re-laying-out a marker is a
legitimate edit; the bracketed upstream code must not move.

## Goals / Non-Goals

**Goals:**

- A uniform, low-maintenance marker layout: the `gaby-vm` token alone on its
  line, reason text as ordinary `//` comments below it.
- A written brace convention for project-authored code, with the
  machine-checkable part wired into `clang-format`.
- Every existing marker and every existing brace violation brought into
  compliance, in one change.
- The two contracts that reviewers rely on — `git grep` enumeration of drift,
  and byte-identity of imported code outside marker regions — preserved
  exactly.

**Non-Goals:**

- Changing marker *semantics*, the reason *wording*, or the `gaby-vm` token.
- Touching upstream (non-marker) comments or code in imported files.
- A wholesale reformat of *tracked* project files with a committed baseline:
  for `test/` and `include/gaby_vm/`, only single-statement brace violations
  are corrected. (The `src/gaby_vm/*.cc` files are new and untracked — bringing
  them under their newly-added `.clang-format` formats them in full, which has
  no committed baseline to churn and keeps the three files mutually
  consistent. A format-on-save hook in this environment reformats any edited
  file under a `DisableFormat: false` config, so a partial reformat there
  would not survive the next edit anyway.)
- Moving `src/gaby_vm/` out from under `src/` (the aspiration noted in
  `src/.clang-format`'s comment) — that is a larger restructure, out of scope.
- Enforcing braces inside imported files (including project-authored code that
  lives inside a marker region in an imported file — it follows the host
  file's upstream style).

## Decisions

### D1 — Marker grammar: token line stands alone

The `gaby-vm` token occupies its own line. The reason follows on the next
line(s) as ordinary `//` comments with no special alignment — they wrap at the
normal comment column like any other comment.

Single-line edit (one changed code line):

```
// gaby-vm:
// <reason, one or more ordinary // lines>
<the single changed code line>
```

Multi-line edit (a changed region, including deletions):

```
// gaby-vm BEGIN:
// <reason, one or more ordinary // lines>
<the changed region; removed upstream code stays commented out here>
// gaby-vm END
```

Rules:

- The token line is *exactly* `// gaby-vm:`, `// gaby-vm BEGIN:`, or
  `// gaby-vm END` (plus leading indentation matching the bracketed code) — no
  reason text trails the token, ever, even for a one-word reason.
- Reason lines are plain `//` comments. They carry no `gaby-vm` token and use
  no alignment beyond the normal comment indent.
- At least one reason line is required for `// gaby-vm:` and
  `// gaby-vm BEGIN:`. `// gaby-vm END` takes no reason.
- No blank line inside the marker block, nor between the block and the code it
  brackets — the bracketed code starts on the line immediately after the last
  reason line.

Terminology: "single-line" and "multi-line" describe the *edit*, not the
comment. A single-line-edit marker can still span several comment lines of
reason text.

*Why token-on-its-own-line over the current inline form:* editing the reason
never reflows anything — continuation lines are plain comments at a fixed
column, so adding a word just wraps normally. The deep alignment to the
`gaby-vm:` column is gone.

*Alternatives considered:* (a) keep the reason inline for short reasons,
own-line only when long — rejected, it reintroduces a judgement call and
inconsistent layout; uniform is the point. (b) Switch to a different or
structured token — rejected, it breaks the grep contract and every doc/spec
reference to `gaby-vm`.

### D2 — The grep-enumeration contract is the regex, and it still holds

The normative enumeration command is unchanged:
`git grep -nE 'gaby-vm( BEGIN| END|:)' src/`. Under the new layout it matches
exactly the token lines (`gaby-vm:`, `gaby-vm BEGIN`, `gaby-vm END`); reason
lines carry no token and are not matched. Output is one line per
single-line-edit marker and two per `BEGIN/END` pair — the same enumeration
the old form produced.

Constraint this imposes on reason text: a reason line must not contain the
literal sequences `gaby-vm:`, `gaby-vm BEGIN`, or `gaby-vm END`, or it would
create a spurious enumeration hit. Hyphenated design-doc filenames such as
`docs/refs/gaby-vm-predecode-cache-design.md` are safe — the regex requires
` BEGIN`, ` END`, or `:` immediately after `gaby-vm`, and `-` matches none of
them. The existing markers already reference that filename in their reason
text without issue.

### D3 — Brace rule: enforce via `clang-format`, scope by directory

The rule: in project-authored code, a single-statement body of
`if` / `else` / `for` / `while` / `do-while` uses braces and a line break — no
one-line bodies.

Scope is **by directory**, not by authorship: `src/gaby_vm/`,
`include/gaby_vm/`, and `test/`. Project-authored code that lives inside a
marker region in an imported file is *not* in scope — it follows the host
file's upstream style so the imported file stays internally consistent.

clang-format settings that enforce the machine-checkable part:

- `InsertBraces: true` — wraps single-statement bodies in `{}`.
- `AllowShortIfStatementsOnASingleLine: Never`.
- `AllowShortLoopsOnASingleLine: false`.
- (`AllowShortBlocksOnASingleLine: Never` is already the Google-base default.)

What clang-format *cannot* fully guarantee: `InsertBraces` is documented by
upstream as semantically incomplete and capable of producing wrong output in
edge cases, so it does not replace review. `conventions.md` states the rule in
prose; the formatter is the first line of enforcement, review is the second.

### D4 — `.clang-format` topology: edit root and `src/.clang-format`, add `src/gaby_vm/.clang-format`

Because clang-format uses the nearest-ancestor config:

- `include/gaby_vm/` and `test/` resolve to the **root** `.clang-format`. The
  brace settings (D3) are added there. The root config keeps its existing VIXL
  copyright header untouched — only config keys are added. The root
  `.clang-format` is repo-root project-owned config, not part of the `src/`
  byte-identity scope, so it needs no marker.
- `src/gaby_vm/` resolves to `src/.clang-format` (`DisableFormat: true`), so
  today it gets no formatting. A new **`src/gaby_vm/.clang-format`** re-enables
  formatting for that subtree:

  ```
  BasedOnStyle: InheritParentConfig
  DisableFormat: false
  SortIncludes: CaseSensitive
  InsertBraces: true
  AllowShortIfStatementsOnASingleLine: Never
  AllowShortLoopsOnASingleLine: false
  ```

  `InheritParentConfig` is a *chained* opt-in: a config inherits its parent
  only if it sets `InheritParentConfig`, and the climb continues upward only
  while each ancestor *also* sets it. `src/.clang-format` originally had no
  `BasedOnStyle`, so the chain would stop there with an LLVM base and never
  reach the root Google config — verified with `clang-format --dump-config`
  (the dump showed `BinPackArguments: true` / `PointerAlignment: Right`, the
  LLVM defaults). So `src/.clang-format` **also** gets
  `BasedOnStyle: InheritParentConfig`. With that, the chain is
  root → `src/` → `src/gaby_vm/`: the innermost file's explicit
  `DisableFormat: false` and `SortIncludes` override the disabling that
  `src/.clang-format` sets, while the Google base and project options flow
  down from the root config — so the brace rule is not hand-duplicated. The
  new file mirrors `src/.clang-format`'s precedent for project-authored config
  files: an explanatory comment, no formal BSD header.

- `src/aarch64/` (imported) still resolves to `src/.clang-format` and stays
  `DisableFormat: true`. Adding `BasedOnStyle: InheritParentConfig` to that
  file is **inert** for imported sources: when `DisableFormat` is true the
  base style is never applied, so inheriting the root Google config changes
  nothing observable. The brace rule never reaches imported files, which is
  exactly the imported-files exemption.

*Why `InheritParentConfig` over a standalone copy of the root config:* a
standalone copy duplicates the Google base, BinPack, Penalty, and Include
settings and would silently drift from the root config over time. Inheriting
means only the keys that actually differ are written down — at the cost of one
inert `BasedOnStyle` line in `src/.clang-format`.

### D5 — Marker rewrite is a layout-only edit of marker regions

Rewriting the 11 existing markers to the D1 layout edits only marker comment
lines, which are themselves part of the marker region — the bracketed upstream
code does not move. The reason *text* is preserved verbatim; only its line
breaks change (re-wrapped onto plain `//` lines at a normal width). Design-doc
path references in reason text must survive the re-wrap intact, since the
"marker reason cites a design document" requirement depends on them.

Verification after the rewrite: `BEGIN` count equals `END` count under the
enumeration grep; every token line stands alone; and `git diff` of
`src/aarch64/` versus pre-change shows only marker comment lines changed.

## Risks / Trade-offs

- **`InsertBraces` misfires on macro-heavy or unusual code** → review the
  `clang-format` diff; treat `InsertBraces` as an aid, not an oracle. The prose
  rule in `conventions.md` is the real contract.
- **`clang-format -i` reformats more than braces** for files not already
  formatter-clean → for tracked files with a committed baseline, apply brace
  fixes only and leave other drift alone. For new/untracked files (the
  `src/gaby_vm/*.cc` set) a full reformat is fine — there is no baseline to
  churn, and a format-on-save hook in this environment would fully reformat
  any such file the moment it is edited regardless.
- **Reason re-wrap accidentally changes wording** → preserve reason text
  word-for-word; only line breaks may change. Spot-check that every
  `docs/refs/*.md` reference still appears.
- **`InheritParentConfig` merge does not behave as expected** → verify with
  `clang-format --dump-config` run against a file inside `src/gaby_vm/` before
  relying on it.
- **A reason line contains a literal marker token** and creates a spurious
  enumeration hit → forbidden by D2; the rewrite must not introduce
  `gaby-vm:` / `gaby-vm BEGIN` / `gaby-vm END` literals into reason prose.

## Migration Plan

1. Update `.clang-format` files: add brace keys to the root config; add
   `src/gaby_vm/.clang-format`. Verify the merged result with
   `clang-format --dump-config` from inside `src/gaby_vm/`.
2. Rewrite the 11 markers in `src/aarch64/` to the D1 layout. Verify with the
   enumeration grep and a byte-identity diff against pre-change.
3. Update the normative spec (`aarch64-simulator`), `docs/conventions.md` (both
   the marker section and the new brace rule), and `docs/architecture.md`.
4. Sweep `src/gaby_vm/` and `test/` for brace-rule violations and fix them.
5. Build and run the test suite under `dev-debug` and `dev-release`; confirm
   green.

Rollback: every edit is localized to the change branch — reverting the branch
fully restores the previous state. No build artifacts or generated files are
involved.
