# Coding conventions

## Formatting

Source is formatted by `clang-format`. The repository-root
[`../.clang-format`](../.clang-format) holds the project style; two subtree
configs refine it — `src/.clang-format` disables formatting for imported VIXL
sources (their byte-identity with upstream is a spec invariant), and
`src/gaby_vm/.clang-format` re-enables the project style for project-authored
code under `src/`.

- Base style: Google.
- `MaxEmptyLinesToKeep: 2` — two blank lines between functions in `.cc`
  files.
- `BinPackArguments: false`, `BinPackParameters: false` — either fit all
  arguments on one line, or one per line. No grouped subsets.
- Include ordering, in priority order:
  1. System headers (`<...>`).
  2. Quoted project headers (`"..."`) that don't match an AArch{32,64}
     pattern.
  3. Quoted AArch32/AArch64 headers (matching `aarch32`/`aarch64` in the
     path).
- Comments containing `NOLINT` are not reformatted.

Avoid bypassing clang-format. If a region really needs hand-formatting, the
usual approach is to wrap it in `// clang-format off` / `// clang-format on`
with a nearby comment explaining why.

## Braces on control-flow statements

In project-authored code — everything under `src/gaby_vm/`,
`include/gaby_vm/`, and `test/` — every `if`, `else`, `for`, `while`, and
`do`-`while` body is wrapped in braces and put on its own line. No single-line
bodies, even for a one-statement `if`:

```cpp
// yes
if (done) {
  return;
}

// no
if (done) return;
```

`clang-format` enforces most of this: the project configuration sets
`InsertBraces: true` to add the braces, plus
`AllowShortIfStatementsOnASingleLine: Never` and
`AllowShortLoopsOnASingleLine: false` to keep the body off the control line.
`InsertBraces` is documented upstream as not fully reliable, so treat it as a
first pass rather than a guarantee — review still catches what the formatter
misses.

Imported VIXL files are exempt. They resolve to `src/.clang-format`
(`DisableFormat: true`) so they stay byte-identical to upstream, and
project-authored code that lives inside a marker region in an imported file
follows that host file's upstream style. The rule is scoped by directory, not
by authorship.

## Identifiers and namespaces

- Project code lives in the `gaby_vm` namespace.
- Imported VIXL code keeps the `vixl` and `vixl::aarch64` namespaces — there
  is no rename. See [`architecture.md`](architecture.md).
- Header guards for project headers: `GABY_VM_<RELATIVE_PATH>_H_`, matching
  the file's path under `include/` or `src/`. Imported VIXL headers keep
  their upstream `VIXL_*_H` guards verbatim.

## License headers

The full BSD 3-Clause header for newly authored source files (C/C++/CMake)
is reproduced at [`README.md`](README.md). Use it verbatim for new files;
for `.cmake`/shell/Python files, swap the leading `//` for `#`.

Imported VIXL files keep their original ARM/VIXL copyright headers
byte-for-byte. Don't rewrite, reformat, or relicense upstream headers. The
upstream licence text lives at [`../LICENSE.vixl`](../LICENSE.vixl), the
top-level [`../LICENSE`](../LICENSE) references it, and
[`../AUTHORS`](../AUTHORS) credits VIXL contributors.

## Marker convention for imported files

Drift from upstream content inside an imported file is recorded with a
`// gaby-vm` marker. Full rules and rationale live in
[`architecture.md`](architecture.md#vixl-import-boundary); the normative
scenarios are in
[`../openspec/specs/aarch64-simulator/spec.md`](../openspec/specs/aarch64-simulator/spec.md).

There are two marker forms. In both, the marker **token sits alone on its
line** and the reason follows on the next line(s) as ordinary `//` comments —
nothing trails the token, and reason lines use no special alignment.

A single-line edit (one changed line) uses a `// gaby-vm:` marker:

```cpp
// gaby-vm:
// Reason for the change, wrapped as ordinary // comment lines.
<the single changed line>
```

A multi-line edit — or a deletion — uses a `// gaby-vm BEGIN:` …
`// gaby-vm END` pair:

```cpp
// gaby-vm BEGIN:
// Reason for the change.
<the changed region; removed upstream code stays commented out here>
// gaby-vm END
```

Rules:

- The token is lowercase `gaby-vm` — never `Gaby-VM` or other variants.
- The token line carries the token only. `// gaby-vm:` and `// gaby-vm BEGIN:`
  always have at least one reason line below them; `// gaby-vm END` takes no
  reason.
- When removing upstream code, leave the deletion commented out inside the
  `BEGIN/END` block so the removal is reviewable.
- `git grep -nE 'gaby-vm( BEGIN| END|:)' src/` enumerates every drifted
  location — keep that true by not writing the literal `gaby-vm:`,
  `gaby-vm BEGIN`, or `gaby-vm END` inside reason prose.

Putting the token on its own line (rather than trailing the reason after it)
keeps edits cheap: adding a word to a reason just rewraps ordinary `//` lines,
with no alignment block to reflow.

## Public API hygiene

Headers under [`../include/gaby_vm/`](../include/gaby_vm/) are designed to be
free of imported VIXL types — they don't include imported VIXL headers or
reference `vixl::*` symbols (including in forward declarations), so the
public surface stays VIXL-free. When a public type needs to expose simulator
state, a `gaby_vm`-namespaced wrapper is the usual approach.
