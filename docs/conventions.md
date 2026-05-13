# Coding conventions

## Formatting

Source is formatted by `clang-format` using the configuration at the
repository root [`../.clang-format`](../.clang-format).

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

Short version: edit imported files with a marker, stick to the lowercase
`gaby-vm` token (not `Gaby-VM` or other variants), and when removing upstream
code leave the deletion commented out inside a `BEGIN/END` block so the
removal is reviewable.

## Public API hygiene

Headers under [`../include/gaby_vm/`](../include/gaby_vm/) are designed to be
free of imported VIXL types — they don't include imported VIXL headers or
reference `vixl::*` symbols (including in forward declarations), so the
public surface stays VIXL-free. When a public type needs to expose simulator
state, a `gaby_vm`-namespaced wrapper is the usual approach.
