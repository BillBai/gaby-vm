# Architecture

This document captures the durable architectural shape of gaby-vm — the bits
that are stable design choices, not implementation details that move every
release. Aspirational direction lives in [`../AGENTS.md`](../AGENTS.md);
detailed normative requirements live in the capability specs under
[`../openspec/specs/`](../openspec/specs/).

## Memory model

The simulator executes against the **embedder's host address space directly**.
Guest pointers are host pointers; guest loads and stores are ordinary host
memory accesses against the embedder's heap, stack, and globals. There is no
MMU, no separate guest physical memory, no address translation, and no virtual
memory model.

The simulator doesn't bounds-check guest accesses. An out-of-bounds or
otherwise invalid guest load/store is just an invalid host memory access; it
will either `SIGSEGV` the embedder process or silently corrupt embedder
state. There's no fault-isolation layer between guest and host.

Consequences for embedders:

- The embedder is responsible for placing guest code and data at host
  addresses the guest expects.
- Lifetime, alignment, and aliasing of any memory the guest touches are the
  embedder's responsibility.
- A misbehaving guest is, by construction, a host-process bug.

This shape suits the project's goals: it keeps the interpreter loop free of
bounds checks (which matters for cache mode), and it lines up
with environments like iOS where shared address space is what an embedder
already has.

## Threading model

The intended embedder usage is **one `Simulator` instance per host (physical)
thread, lazily initialised** on first use on that thread. The simulator itself
is not heavily multi-threaded internally and is not designed to be shared
across threads — per-instance state (registers, system registers, internal
scratch) is owned by the thread that constructed it.

gaby-vm doesn't currently provide thread-local plumbing; embedders place the
per-thread instance themselves (e.g. behind `thread_local`, a TLS slot, or
their own thread-context structure).

Implication for things we add later:

- New per-instance state (the predecode cache, ancillary tables, etc.)
  should be cheap enough to duplicate per thread, or designed as
  shared-and-read-only / atomically-updated so that thread-local instances
  can refer to it safely.
- Cross-thread mutable global state doesn't fit this model and we'll avoid
  introducing it.
- The embedding API aims to not require cross-thread synchronisation on the
  gaby-vm side.

## VIXL import boundary

gaby-vm is a **standalone project**, not an in-place fork of VIXL. The
reference VIXL source tree is expected adjacent at `../vixl` for study; only a
bounded subset is imported into this repository.

- **Shipping import scope** (`Sources/gaby_vm/src/`) is Tiers 1, 2, and 3 of
  [`refs/vixl-extraction-map.md`](refs/vixl-extraction-map.md). Tier 0
  (assembler, macro-assembler, code-buffer, pool-manager, AArch32) is **not**
  imported into the shipping library, since gaby-vm is a *consumer* of
  instruction bytes — it doesn't generate code. If a later capability needs
  something out of Tier 0, the extraction map gets updated alongside the import.
- **Test-only Tier-0 copy.** One narrow exception lives *outside* the shipping
  tree, under `test/test_support/vixl_asm/`: a copy of the Tier-0 VIXL assembler
  + macro-assembler + code-buffer (and the VIXL test infra), pinned to the same
  import SHA, used by the `vixl_port` suite to live-assemble upstream test bodies
  at test time. It compiles into a `gaby_vm_vixl_asm_testonly` library that is
  never linked into `gaby_vm::gaby_vm` (no `::` alias, `_testonly` suffix,
  PRIVATE-linked, gated on `GABY_VM_BUILD_TESTS`), builds `VIXL_CODE_BUFFER_MALLOC`
  only (no executable-memory path), and so does not weaken the "consumer, not
  generator" / no-RWX boundary of the shipping product. See
  [`testing.md`](testing.md#ported-vixl-tests-vixl_port).
- **Layout** mirrors upstream byte-for-byte under `Sources/gaby_vm/src/`:
  shared root files (e.g. `utils-vixl.h`, `cpu-features.h`) at
  `Sources/gaby_vm/src/`; AArch64-specific files at
  `Sources/gaby_vm/src/aarch64/`. There's no `third_party/vixl/` tree.
- **Marker convention** is how we record drift from upstream content. The
  marker token sits alone on its line; the reason follows on the next
  ordinary `//` comment lines. There are two forms: a single-line
  `// gaby-vm:` marker immediately above the changed line, or a multi-line
  `// gaby-vm BEGIN:` … `// gaby-vm END` block. The token `gaby-vm` is
  lowercase; the goal is that
  `git grep -nE 'gaby-vm( BEGIN| END|:)' Sources/gaby_vm/src/`
  enumerates every drift, so reviewers can audit modifications cheaply.
  Detailed scenarios are in
  [`../openspec/specs/aarch64-simulator/spec.md`](../openspec/specs/aarch64-simulator/spec.md).

## Namespace and dispatch structure

- We keep the imported `vixl` and `vixl::aarch64` namespaces rather than
  renaming them to `gaby_vm::aarch64`. Renaming would touch a lot of imported
  code without buying us anything yet.
- The upstream `Decoder → VisitNamedInstruction → leaf` dispatch flow is
  preserved structurally. Class declarations for `Simulator`, `Decoder`,
  `Instruction` and friends keep their upstream member variables and method
  signatures; modifications and additions go inside marker regions so they
  stay reviewable.
- gaby-vm runs in two execution modes (terminology in
  [`conventions.md`](conventions.md#terminology)):
  - **decoder mode** — drives the imported simulator along the upstream
    flow above. This is the historic path and the bench harness default.
  - **cache mode** — drives `gaby_vm::Simulator` over a `PredecodeCache`,
    predecoding once at code-range registration and dispatching cached
    entries on the steady-state loop. Normative behaviour lives in the
    `predecode-cache` and `benchmark-harness` capability specs under
    [`../openspec/specs/`](../openspec/specs/).

## Public API surface

Public headers live under
[`../Sources/gaby_vm/include/gaby_vm/`](../Sources/gaby_vm/include/gaby_vm/) and
own the `gaby_vm::` namespace. They are designed not to expose imported VIXL
types — they don't include imported VIXL headers and don't reference
`vixl::*` symbols, so embedders see only the `gaby_vm::` surface.

Imported VIXL headers are reached from inside the `gaby_vm` static library
and from privileged in-tree test targets, via a `PRIVATE` include of
`${PROJECT_SOURCE_DIR}/Sources/gaby_vm/src` plus the same `VIXL_*` compile
defines that gate
the imported sources. Consumers linking against `gaby_vm::gaby_vm` don't
inherit those defines or include paths.
