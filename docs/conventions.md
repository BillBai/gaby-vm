# Coding conventions

## Formatting

Source is formatted by `clang-format`. The repository-root
[`../.clang-format`](../.clang-format) holds the project style; two subtree
configs refine it — `Sources/gaby_vm/src/.clang-format` disables formatting
for imported VIXL sources (their byte-identity with upstream is a spec
invariant), and `Sources/gaby_vm/src/gaby_vm/.clang-format` re-enables the
project style for project-authored code under `Sources/gaby_vm/src/`.

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

In project-authored code — everything under `Sources/gaby_vm/src/gaby_vm/`,
`Sources/gaby_vm/include/gaby_vm/`, and `test/` — every `if`, `else`, `for`, `while`, and
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

Imported VIXL files are exempt. They resolve to
`Sources/gaby_vm/src/.clang-format`
(`DisableFormat: true`) so they stay byte-identical to upstream, and
project-authored code that lives inside a marker region in an imported file
follows that host file's upstream style. The rule is scoped by directory, not
by authorship.

## Identifiers and namespaces

- Project code lives in the `gaby_vm` namespace.
- Imported VIXL code keeps the `vixl` and `vixl::aarch64` namespaces — there
  is no rename. See [`architecture.md`](architecture.md).
- Header guards for project headers: `GABY_VM_<RELATIVE_PATH>_H_`, matching
  the file's path under `Sources/gaby_vm/include/` or `Sources/gaby_vm/src/`.
  Imported VIXL headers keep their upstream `VIXL_*_H` guards verbatim.

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
- `git grep -nE 'gaby-vm( BEGIN| END|:)' Sources/gaby_vm/src/` enumerates
  every drifted location — keep that true by not writing the literal `gaby-vm:`,
  `gaby-vm BEGIN`, or `gaby-vm END` inside reason prose.

Putting the token on its own line (rather than trailing the reason after it)
keeps edits cheap: adding a word to a reason just rewraps ordinary `//` lines,
with no alignment block to reflow.

## Public API hygiene

Headers under
[`../Sources/gaby_vm/include/gaby_vm/`](../Sources/gaby_vm/include/gaby_vm/) are
designed to be free of imported VIXL types — they don't include imported VIXL headers or
reference `vixl::*` symbols (including in forward declarations), so the
public surface stays VIXL-free. When a public type needs to expose simulator
state, a `gaby_vm`-namespaced wrapper is the usual approach.

## Terminology

gaby-vm has two execution paths through the simulator. They are referred
to throughout the codebase by these names, and prose, code, CLI, output
keys, and specs all use the same word:

- **decoder mode** — execution drives the imported `vixl::aarch64::Simulator`
  along upstream VIXL's `Decoder → VisitNamedInstruction → leaf` dispatch
  path. This is the historic path and the bench harness default.
- **cache mode** — execution drives `gaby_vm::Simulator` over a
  `gaby_vm::PredecodeCache`: instructions are predecoded once at code-range
  registration time, and the timed loop dispatches off cached entries
  without re-walking the upstream decoder.

The two labels (`decoder`, `cache`) and the noun (`mode`) are the
canonical vocabulary. They double as the values of the bench harness's
`--mode {decoder|cache}` flag and as the `mode:` key in benchmark output,
so prose and tooling resolve to the same identifiers.

### Identifiers stay in English

The labels do **not** get translated when the surrounding prose uses another
human language. In Chinese prose, write the English identifier plus the local
noun for "mode"; do not translate `decoder` or `cache`. The reason is
mechanical: a reader or `git grep` search for `decoder` or `cache` should land
on every relevant location regardless of the surrounding language. Translating
the identifier defeats that.

The noun **mode** translates normally because it is a description, not an
identifier.

### Forbidden flutter phrasings

These near-synonyms used to drift through docs; they are no longer
allowed when referring to the *modes*:

- "decoder engine" / "cache engine" / "execution engine" — replaced by
  "decoder mode" / "cache mode" / "execution mode".
- "cache-on" / "cache-off" — replaced by "cache mode" / "decoder mode".
- "cached dispatch path" / "predecode/dispatch cache" used as a *mode
  name* — replaced by "cache mode".

The data structure's own name `PredecodeCache` is preserved (it is a
class, not a mode), and mechanism-level phrasing like "predecode once,
execute the cached path repeatedly" stays — it describes how cache mode
works, it is not a label for the mode itself.
