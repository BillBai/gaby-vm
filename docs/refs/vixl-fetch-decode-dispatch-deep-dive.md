# VIXL AArch64 Simulator Fetch, Decode, and Dispatch Deep Dive

[Chinese version](vixl-fetch-decode-dispatch-deep-dive.zh-cn.md)

This working note explains the path the upstream VIXL AArch64 simulator takes
for one instruction, why that path is expensive, and which risks a predecode
cache must handle when it bypasses the decoder. For factual file/line references
to the decoder code, also read `vixl-decode-dispatch-pattern.md`.

## 1. Whole Chain

One upstream simulator instruction roughly follows this chain:

```text
Simulator::RunFrom(first_pc)
  -> Simulator::Run()
     while (!IsSimulationFinished()):
       -> Simulator::ExecuteInstruction()
          pc_modified_ = false
          BType / guarded page check
          last_instr_was_movprfx = ...
          -> Decoder::Decode(pc_)
             -> CompiledDecodeNode::Decode(instr)
                tree walk: bit extract, table lookup, recursion
                -> Decoder::VisitNamedInstruction(instr, "<form>")
                   Metadata m = {{"form", name}}
                   form_hash = Hash(name.c_str())
                   unallocated-form lookup
                   visitor list fan-out
                      -> CPUFeaturesAuditor::Visit
                      -> Simulator::Visit
                         form_hash_ = Hash(form.c_str())
                         FormToVisitorFnMap lookup
                         -> leaf function
          last_instr_ = ReadPc()
          IncrementPc()
          LogAllWrittenRegisters()
          UpdateBType()
          CPUFeaturesAuditor final check
```

For a small instruction, the simulator does several member-function-pointer
calls through the decode tree, a visitor-list walk, string hashing, map lookups,
and at least one heap allocation before the semantic leaf starts doing the real
register work.

## 2. One `ADD x0, x1, #4`

### 2.1 Fetch: PC Is a Host Pointer

VIXL does not model a separate guest address space. `pc_` is a
`const Instruction*` that points at a 32-bit instruction word in the host
process. `Instruction::GetNextInstruction()` is plain pointer arithmetic:
`this + kInstructionSize`.

That fact makes a cache practical. A cache can use `const Instruction*` or its
integer address directly. It does not need guest-to-host translation.

The same direct-host-pointer model applies to memory. Guest code and data are
ordinary readable/writable host memory from the simulator's point of view. The
cache therefore assumes registered code bytes do not change.

### 2.2 Entering `ExecuteInstruction`

The upstream `ExecuteInstruction` clears `pc_modified_`, checks BType/guarded
page state, computes whether the previous instruction was MOVPRFX, then runs
the decoder.

`pc_modified_` is load-bearing. Branch leaves call `WritePc`, which sets it.
The end-of-instruction `IncrementPc()` only advances by 4 when no branch wrote
PC. Any cache path must preserve that behavior.

BType and guarded-page checks depend on runtime state, so they cannot be cached
as a simple per-form property.

### 2.3 `Decoder::Decode`: Tree Walk

`Decoder::Decode` forwards to a compiled decode tree. Each non-leaf node:

1. extracts a set of instruction bits through a member-function pointer;
2. indexes a decode table;
3. recurses to the next node.

The root table samples 10 bits and has 1024 entries. Middle nodes are much
smaller. A simple add-immediate instruction usually reaches a form leaf after a
few nodes.

### 2.4 `Decoder::VisitNamedInstruction`: Visitor Fan-Out

At the leaf, the decoder calls `VisitNamedInstruction(instr, "<form>")`. That
function builds a `Metadata` unordered map with the form name, hashes the form
string, checks unallocated-form masks, then walks the visitor list.

The default visitor list includes CPUFeaturesAuditor and Simulator. Trace adds a
disassembler visitor. Custom visitors can also be appended.

Costs here include:

- heap allocation for the unordered-map bucket storage;
- string hashing;
- multimap lookup for unallocated encodings;
- linked-list iteration and virtual calls.

### 2.5 `Simulator::Visit`: Hash Again, Lookup Again

`Simulator::Visit` reads the string form from `Metadata`, hashes the same form
name again into `form_hash_`, finds the leaf in `FormToVisitorFnMap`, then calls
the leaf.

`form_hash_` is not just diagnostic state. Shared `Simulate_*` leaves use it to
select a concrete behavior. A cache path that jumps directly to a leaf must
write `form_hash_` before the call.

### 2.6 Leaf Work Still Re-Extracts Operands

For add-immediate, `VisitAddSubImmediate` computes the immediate and calls
`AddSubHelper`. The visit function and helper extract fields such as
`imm12`, shift, `sf`, flags, Rn, Rn mode, Rd, and Rd mode from the instruction
word. These operations are small, but they repeat on every execution.

V1 keeps VIXL's leaf code unchanged. Operand pre-extraction is a later
optimization.

### 2.7 Exit: PC and Auditor

After the leaf returns, upstream code:

- validates MOVPRFX chaining when needed;
- records `last_instr_`;
- increments PC if `pc_modified_` is false;
- logs written registers;
- updates BType;
- checks CPUFeaturesAuditor.

These steps are part of VIXL's observable simulator behavior. Cache mode may
move feature auditing to registration time, but the PC and runtime-state updates
must remain equivalent.

### 2.8 Cost Sketch

For a small scalar instruction, a rough cost model is:

| Stage | Approximate cost |
|-------|------------------|
| Decode tree walk | several indirect calls and table loads |
| `VisitNamedInstruction` | metadata allocation, string hash, unallocated lookup, visitor fan-out |
| `Simulator::Visit` | second string hash, map lookup, function-pointer call |
| Leaf operand extraction | several bitfield operations |
| Real semantics | usually a small number of register reads, arithmetic operations, and writes |

The semantic work can be only a small fraction of the total for simple
instructions. The cache exists to remove the repeated "find the leaf" work.

## 3. Bottleneck Accounting

### 3.1 Rough Per-Instruction Cycle Split

The exact number depends on branch prediction, cache state, and workload mix,
but the order of magnitude is clear:

| Operation | Rough cycle range | Notes |
|-----------|------------------:|-------|
| L1 data-cache load | ~4 | decode tables, instruction word, registers |
| Predicted indirect call | ~5 | decode tree, visitors, leaf |
| Mispredicted indirect call | ~15-25 | workload dependent |
| unordered-map lookup | ~30-80 | hash plus bucket walk |
| string hash and copy | ~20-50 | form name handled twice |
| heap allocation | ~50-200 | metadata map bucket storage |
| virtual call and list walk | ~10 each | visitor chain |
| leaf arithmetic | ~5-10 | for a simple ADD |

This is why VIXL is a clean reference-style simulator rather than a
performance interpreter.

### 3.2 Hotspot Priority

Highest-value work:

1. Remove `VisitNamedInstruction` from the steady-state path.
2. Remove compiled-decode-tree walking from the steady-state path.
3. Remove repeated form-hash lookup from `Simulator::Visit`.
4. Later, reduce repeated operand extraction inside leaves.
5. Preserve or pre-validate CPU feature checks.

### 3.3 Cacheable vs Runtime State

| Item | Cacheable? | Notes |
|------|------------|-------|
| Decode-tree result, leaf function | yes | same instruction word maps to the same form |
| Form hash | yes | pure function of form name |
| Metadata allocation | yes, by removal | cache path does not need Metadata |
| Visitor fan-out | partly | trace/debug/custom visitors require decoder mode |
| Operand fields | later | needs per-form fast paths |
| BType / guarded page | no | runtime state |
| `pc_modified_` and PC increment | no | runtime state |
| MOVPRFX previous-instruction state | no | cross-instruction state |
| Leaf semantics | no | the actual work |

### 3.4 Secondary Effects

The upstream path also stresses:

- the branch predictor, through many indirect calls per instruction;
- I-cache, through decoder, visitor, and leaf code all being hot;
- D-cache, through decode tables, visitor list nodes, and maps;
- the allocator, through per-instruction metadata allocation.

A cache hit reduces most of that to range lookup plus one leaf call.

## 4. Predecode Cache Design Insights

This section records the design ideas that fed into
`gaby-vm-predecode-cache-design.md`. Some early details, such as page buckets
and 8-byte thunk entries, were superseded by the implemented V1 range-table and
16-byte entry design.

### 4.1 Foundation

`pc_` is a host pointer and instruction words are fixed 4-byte units. That lets
the cache use flat range indexing:

```text
entry = range.entries[(pc - range.start) / 4]
```

### 4.2 Cache Entry

The early proposal used a slot with `leaf_fn`, `form_hash`, and a state field.
The implemented V1 entry stores:

```cpp
uint32_t form_hash;
uint32_t flags;
const void* leaf;
```

The important invariant stayed the same: cache mode writes `form_hash_` before
calling the cached leaf.

### 4.3 Storage Shape

Early options included a global hash table, per-page buckets, or per-region
flat arrays. V1 chose registered `CodeRange` objects with flat entry arrays.
This keeps the hot path simple and fits the embedder contract: code ranges are
registered explicitly before cache execution.

### 4.4 Hot Path

The implemented cache path has this shape:

```cpp
void Simulator::ExecuteInstructionCached() {
  VIXL_ASSERT(IsWordAligned(pc_));
  VIXL_ASSERT(cache_ != nullptr);
  pc_modified_ = false;

  const PredecodedEntry* entry = LookupEntryOrAbort(pc_);

  if (entry->flags & kBtiRelevant) {
    RunImportedBTypeCheck();
  }

  bool last_instr_was_movprfx =
      (form_hash_ == "movprfx_z_z"_h) ||
      (form_hash_ == "movprfx_z_p_z"_h);

  form_hash_ = entry->form_hash;
  CallCachedLeaf(entry->leaf, pc_);

  if (last_instr_was_movprfx) {
    VIXL_CHECK(pc_->CanTakeSVEMovprfx(form_hash_, last_instr_));
  }

  last_instr_ = ReadPc();
  IncrementPc();
  LogAllWrittenRegisters();
  UpdateBType();
}
```

It intentionally does not fall back to decoder mode on cache misses. Out-of-range
PC aborts.

### 4.5 Populate Path

`RegisterCodeRange` decodes the range once, resolves each form to a leaf, fills
the entry array, and installs a stable `CodeRange`. Structural failures leave
the cache unchanged.

Unallocated or data-in-stream words are represented by sentinel entries instead
of rejecting the whole range. Executing such a sentinel aborts.

### 4.6 V2: Operand Pre-Extraction

V1 still calls existing VIXL leaves, so leaves re-extract fields from the
instruction word. A later version can specialize forms and pre-extract expensive
derived operands. The current `flags` field and the 16-byte entry shape leave
room for small per-form data without changing the public API.

## 5. Risks and Challenges

### R1: `form_hash_` Must Be Restored Before Calling a Leaf

Shared `Simulate_*` leaves switch on `form_hash_`. Cache mode writes the cached
form hash before calling the leaf. Tests must cover multiple forms that share a
leaf.

### R2: MOVPRFX Chaining

SVE MOVPRFX requires the previous instruction and current form hash to be
tracked correctly. Cache mode keeps the same `last_instr_` update and computes
`last_instr_was_movprfx` before writing the new form hash.

### R3: CPU Features Auditor

Cache mode bypasses the visitor chain, so auditing cannot rely on per-instruction
visitor execution. V1 audits during registration and does not run the auditor in
the hot path.

### R4: Trace, Debugger, and Custom Visitors

Those features need the full visitor chain. They belong to decoder mode rather
than the cache mode API.

### R5: BType and Guarded Pages

BType is runtime state. Cache mode preserves the imported check and gates it
with a predecoded BTI-relevant flag.

### R6: Self-Modifying Code

The cache assumes registered code bytes are immutable. V1 has no invalidation
API.

### R7: Multiple Simulator Instances

The cache is shared and append-only. Registration is serialized, while the
current-range hit path is lock-free.

### R8: Cache Size

Cache memory grows with registered code size. V1 leaves budget control to the
embedder.

### R9: iOS and no-JIT Compliance

Predecoded entries are ordinary data. Leaf pointers refer to functions already
compiled into the binary. The cache does not allocate executable memory or call
`mprotect(PROT_EXEC)`.

### R10: Correctness Regressions

Cache mode must reproduce decoder-mode architectural effects. ShadowRunner
compares the two modes per instruction.

### R11: Branches, Returns, and Indirect Targets

The cache accelerates already-registered PCs. Indirect branches still resolve
their target at runtime; the next step performs the normal range lookup.

### R12: Unallocated and Unimplemented Encodings

Unallocated and feature-gated words may be data embedded in a code range.
Rejecting the entire range would lock such functions out of cache mode. V1 uses
sentinel entries: registration succeeds, normal code can branch around the data,
and execution at the sentinel PC aborts with a useful address. VIXL-known but
unimplemented forms use a separate unimplemented sentinel that aborts with form
detail.

## 6. Related Docs

- [`vixl-decode-dispatch-pattern.md`](./vixl-decode-dispatch-pattern.md):
  factual file/line reference for the decode path.
- [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md):
  simulator subsystem overview.
- [`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md):
  authoritative implemented cache design.
- [`vixl-extraction-map.md`](./vixl-extraction-map.md):
  imported-file map.
