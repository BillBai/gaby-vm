# Gaby-VM Dispatch Redesign Notes - 2026-06-02

> **What this is:** a rough directional note. It came from the Phase 1 design
> discussion in
> [`gaby-vm-cache-hotpath-profile-2026-06-02.md`](../profiles/gaby-vm-cache-hotpath-profile-2026-06-02.md):
> while discussing dispatch contraction and operand predecode, it became clear
> that those phases were patching around a half-finished execution model. This
> note preserves the larger "replace the model" idea.
>
> **What this is not:** not a final design, not an OpenSpec change, and not a
> schedule. It was explicitly not the immediate work item. If this direction is
> revived, start a fresh design from it.

Related current-tier documents:

- [`gaby-vm-predecode-cache-design.md`](../../refs/gaby-vm-predecode-cache-design.md)
- [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)

## 1. Diagnosis: The Current Cache Is Only Half A Cache

The current `predecode once -> cache -> execute` path only caches the first
dispatch level: form to leaf function. The leaf still re-decodes sub-forms and
extracts operands every time the instruction executes.

Example: `VisitLogicalShifted` still extracts `reg_size`, shift kind, shift
amount, `Rm`, and `Rn`, then switches again on `LogicalOpMask` to pick
`AND` / `ORR` / `EOR` / `ANDS`. The cache only gets the program to
`VisitLogicalShifted`; it does not eliminate the per-execution operand decode or
the second dispatch.

The same pattern exists in two shapes:

- `Visit*` leaves, where secondary dispatch usually uses `instr->Mask(...)`.
- `Simulate_*` leaves, where secondary dispatch often switches on
  `form_hash_`.

The profile matched this diagnosis: a large share of ALU leaf time was still
spent pulling fields out of `Instruction`. Bytecode interpreters such as Lua or
JS engines usually do not pay this repeated decode cost because their bytecode
is already decoded into execution operands.

## 2. Target Model: A Decoded, Threadable Interpreter

The ideal entry is self-contained: a function pointer plus pre-extracted
operands.

```c
struct DecodedEntry {
  void (*fn)(const DecodedEntry* e, Sim* s);
  uint8_t rd;
  uint8_t rn;
  uint8_t rm;
  uint32_t imm;
};
```

The leaf no longer reads `form_hash_` or re-decodes operand bits from the
instruction:

```c
void leaf_and_shifted(const DecodedEntry* e, Sim* s) {
  s->x[e->rd] = s->x[e->rn] & shift(s->x[e->rm], e->shift, e->amount);
}
```

The second dispatch and operand extraction move into predecode. Runtime becomes
`entry->fn(entry, sim)`.

Important consequences:

- `form_hash_` eventually leaves the hot path. It remains during migration for
  trampoline entries that still call the old VIXL leaf path.
- Hot-path virtual dispatch goes away because new leaves can be free/static
  functions taking an explicit simulator pointer.
- The shape naturally supports a threaded interpreter later:
  `fn(entry*, sim*)` can tail-call the next entry.

## 3. Guardrail: This Must Not Become A New IR

This redesign touches the boundary set by the project rules: no new complex IR,
keep VIXL leaf semantics where practical, and do not rewrite leaf semantics just
to clean up the cache.

The allowed line:

- One guest instruction maps to one decoded entry.
- Predecode selects the exact leaf.
- Predecode stores operands in that entry.
- A next pointer may be used for threading.

The forbidden line:

- Cross-instruction analysis.
- Basic-block merging.
- Constant folding.
- Dead-code elimination.
- Register allocation.
- Instruction reordering.
- Any optimization that breaks "one guest instruction equals one entry".

The invariant is: one guest instruction, one entry, one leaf call, unchanged
semantics.

Specialized leaves should reuse the same VIXL arithmetic, lane loops, shifting,
saturation, rounding, and state updates. The change is how operands reach the
leaf and how the leaf is selected.

## 4. Hard Design Points

### Entry size

The current entry fits in 16 bytes. Pre-extracted operands do not.

Possible layouts:

- A wider self-contained entry, likely 32 or 64 bytes.
- A 16-byte entry plus side-table index.
- A union layout keyed by leaf type.

The rough preference was a wider self-contained entry because it gives the
threaded path better locality, but this decision sets the cache memory model and
was intentionally left open.

### Leaf specialization count

Full specialization can explode code size. For example, logical shifted
instructions contain operation, invert, register-size, and shift axes. Fully
expanding every axis across all forms may create thousands of leaves.

The practical approach is to specialize only the expensive or branch-heavy axes
and leave other fields as entry data. The goal is to remove the costly repeated
decode, not to specialize every possible bit.

### Simulator inheritance

Removing `Simulator : DecoderVisitor` is not the source of the performance win.
Once the hot path stops calling `Visit*`, the virtual inheritance layer is not on
the hot path. Keeping it preserves the decoder track, which is valuable for
`ShadowRunner` and other differential checks.

Recommended direction: move hot leaves to free/static functions, but keep
`Simulator` as a `DecoderVisitor` until the new model is proven and the decoder
track has a replacement oracle.

## 5. Low-Risk Migration Path

Do not rewrite everything at once.

1. Add the new entry layout and `void (*)(const Entry*, Sim*)` leaf signature.
2. Write specialized leaves only for the hottest forms first.
3. Attach a trampoline leaf to all unmigrated forms. The trampoline writes
   `form_hash_` and calls the existing member-function leaf.
4. Let predecode decide whether each instruction gets a specialized leaf or a
   trampoline.
5. Measure and use `ShadowRunner` to compare the new leaves instruction by
   instruction.

In this migration, `form_hash_` is not removed on day one. It remains for the
trampoline path and disappears only if the long tail is migrated.

## 6. Status

This was only recorded, not approved for immediate implementation. The previous
roadmap Phase 1 and Phase 2 were paused. The likely next step at the time was a
real Lua/JS head-to-head benchmark to decide whether this redesign was worth
the complexity and where to start.

Open decisions before a formal design:

1. Scope: a small validation slice or a full model design first.
2. Entry layout: wider entry, 16-byte plus side table, or union layout.
3. Inheritance: keep decoder track as oracle or remove it and replace the
   oracle first.
4. Threading timing: first slice or later step.

## 7. Index

- Triggering profile and roadmap:
  [`gaby-vm-cache-hotpath-profile-2026-06-02.md`](../profiles/gaby-vm-cache-hotpath-profile-2026-06-02.md)
- Current cache design:
  [`gaby-vm-predecode-cache-design.md`](../../refs/gaby-vm-predecode-cache-design.md)
- Relevant source anchors at the time:
  `Sources/gaby_vm/src/aarch64/simulator-aarch64.h`,
  `Sources/gaby_vm/src/aarch64/simulator-aarch64.cc`,
  `Sources/gaby_vm/include/gaby_vm/predecode_cache.h`,
  `Sources/gaby_vm/src/gaby_vm/predecode_cache.cc`, and
  `shadow_runner.cc`.
