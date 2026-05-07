# VIXL Decode/Dispatch Pattern (per-instruction control flow)

> The exact path a single 32-bit AArch64 instruction word travels from
> the simulator's main loop to the function that updates guest state.
> Citations are paths inside `../vixl/`. The Gaby-VM predecode cache
> short-circuits most of this path; this doc establishes what the cache
> replaces and what it must still preserve.

## End-to-end picture

```
Simulator::RunFrom(first)            // simulator-aarch64.cc:840
   |
   v
Simulator::Run()                     // simulator-aarch64.cc:821
  while !IsSimulationFinished():
   |
   v
Simulator::ExecuteInstruction()      // simulator-aarch64.h:1401
  pc_modified_ = false
  ... BType guard checks ...
   |
   v
Decoder::Decode(pc_)                 // decoder-aarch64.cc:39
   |
   v
CompiledDecodeNode::Decode(instr)    // decoder-aarch64.cc:1354
  recursive tree walk via member-fn-ptr extractors
  leaf -> Decoder::VisitNamedInstruction(instr, name)
   |
   v
Decoder::VisitNamedInstruction       // decoder-aarch64.cc:137
  build Metadata { "form": name }    // <- per-instruction heap alloc
  hash form name -> form_hash
  consult form_to_unalloc_ map
  for each visitor in visitors_ list:
     visitor->Visit(metadata, instr) // virtual dispatch
       |
       (auditor)
       (PrintDisassembler if trace)
       (Simulator)
       v
Simulator::Visit(metadata, instr)    // simulator-aarch64.cc:2306
  re-hash metadata["form"] -> form_hash_
  GetFormToVisitorFnMap()->find(form_hash_)
  call (it->second)(this, instr)
   |
   v
Simulator::VisitXXX(instr) /         // e.g. VisitAddSubImmediate cc:4182
Simulator::Simulate_*(instr)         //      Simulate_PdT_PgZ_ZnT_ZmT cc:2325
   reads operand fields off instr (raw 32-bit word)
   reads/writes registers, may call WritePc()
   |
   v
back to ExecuteInstruction post-decode steps
  IncrementPc()  // skip if pc_modified_
  LogAllWrittenRegisters()
  UpdateBType()
  cpu_features_auditor_.InstructionIsAvailable() assert
```

The next sections walk each layer in order.

## 1. The execution loop

`simulator-aarch64.cc:840-843`:

```cpp
void Simulator::RunFrom(const Instruction* first) {
  WritePc(first, NoBranchLog);
  Run();
}
```

`Run()` body at `simulator-aarch64.cc:821-836`:

```cpp
if (debugger_enabled_) {
  // Slow path to check for breakpoints only if the debugger is enabled.
  Debugger* debugger = GetDebugger();
  while (!IsSimulationFinished()) {
    if (debugger->IsAtBreakpoint()) {
      fprintf(stream_, "Debugger hit breakpoint, breaking...\n");
      debugger->Debug();
    } else {
      ExecuteInstruction();
    }
  }
} else {
  while (!IsSimulationFinished()) {
    ExecuteInstruction();
  }
}
```

`IsSimulationFinished()` returns `pc_ == kEndOfSimAddress`
(`simulator-aarch64.h:1362`); `kEndOfSimAddress = NULL`
(`simulator-aarch64.cc:58`); `ResetRegisters()` writes that into LR
(`cc:720`) so the convention is "RET to NULL exits the simulator".

## 2. ExecuteInstruction (the per-instruction step)

The whole step is inline at `simulator-aarch64.h:1401-1442`:

```cpp
void ExecuteInstruction() {
  // The program counter should always be aligned.
  VIXL_ASSERT(IsWordAligned(pc_));
  pc_modified_ = false;

  // On guarded pages, if BType is not zero, take an exception on any
  // instruction other than BTI, PACI[AB]SP, HLT or BRK.
  if (PcIsInGuardedPage() && (ReadBType() != DefaultBType)) {
    if (pc_->IsPAuth()) {
      Instr i = pc_->Mask(SystemPAuthMask);
      if ((i != PACIASP) && (i != PACIBSP)) {
        VIXL_ABORT_WITH_MSG("Executing non-BTI instruction with wrong BType.");
      }
    } else if (!pc_->IsBti() && !pc_->IsException()) {
      VIXL_ABORT_WITH_MSG("Executing non-BTI instruction with wrong BType.");
    }
  }

  bool last_instr_was_movprfx =
      (form_hash_ == "movprfx_z_z"_h) || (form_hash_ == "movprfx_z_p_z"_h);

  // decoder_->Decode(...) triggers at least the following visitors:
  //  1. The CPUFeaturesAuditor (`cpu_features_auditor_`).
  //  2. The PrintDisassembler (`print_disasm_`), if enabled.
  //  3. The Simulator (`this`).
  // User can add additional visitors at any point, but the Simulator requires
  // that the ordering above is preserved.
  decoder_->Decode(pc_);

  if (last_instr_was_movprfx) {
    VIXL_ASSERT(last_instr_ != NULL);
    VIXL_CHECK(pc_->CanTakeSVEMovprfx(form_hash_, last_instr_));
  }

  last_instr_ = ReadPc();
  IncrementPc();
  LogAllWrittenRegisters();
  UpdateBType();

  VIXL_CHECK(cpu_features_auditor_.InstructionIsAvailable());
}
```

State management around the `decoder_->Decode(pc_)` call:

- **Before:** `pc_modified_ = false` (h:1404). The BType-on-guarded-pages
  check (h:1408-1418) aborts on BTI violations.
- **After:**
  - MOVPRFX rule (h:1431-1434) — checks the previous SVE instruction's
    form hash matches the current one, if it was a MOVPRFX.
  - `last_instr_ = ReadPc()` (h:1436) — saved for next iteration's
    MOVPRFX check.
  - `IncrementPc()` (h:1437) — advances `pc_` by 4 bytes *iff*
    `pc_modified_` is still false (`simulator-aarch64.h:1379-1383`):
    ```cpp
    void IncrementPc() {
      if (!pc_modified_) {
        pc_ = pc_->GetNextInstruction();
      }
    }
    ```
  - `LogAllWrittenRegisters()` (h:1438) — emits trace records for
    registers the leaf changed.
  - `UpdateBType()` (h:1439) — promotes `next_btype_` to `btype_`.
  - `VIXL_CHECK(cpu_features_auditor_.InstructionIsAvailable())` (h:1441)
    — aborts execution if the auditor saw an instruction the
    configured CPU profile can't run.

## 3. The Decoder

`Decoder::Decode(const Instruction*)` at `decoder-aarch64.cc:39-46`:

```cpp
void Decoder::Decode(const Instruction* instr) {
  std::list<DecoderVisitor*>::iterator it;
  for (it = visitors_.begin(); it != visitors_.end(); it++) {
    VIXL_ASSERT((*it)->IsConstVisitor());
  }
  VIXL_ASSERT(compiled_decoder_root_ != NULL);
  compiled_decoder_root_->Decode(instr);
}
```

The leading loop is a debug-only assertion; the actual dispatch is one
call into `compiled_decoder_root_->Decode(instr)`. Decoder members
(`decoder-aarch64.h:320-413`):

- `std::list<DecoderVisitor*> visitors_` — the chain of visitors
  registered onto this decoder.
- `CompiledDecodeNode* compiled_decoder_root_` — root of the precompiled
  dispatch tree.
- `std::map<std::string, DecodeNode> decode_nodes_` — every named decode
  node, keyed by name.
- A `form_to_unalloc_` multimap for the per-form unallocated mask/value
  pairs.

Visitor list management methods (`decoder-aarch64.cc:90-135`):

- `AppendVisitor` (cc:90), `PrependVisitor` (cc:95),
  `InsertVisitorBefore`/`After` (cc:100, 116), `RemoveVisitor`
  (cc:133). Order matters — Simulator's constructor relies on
  auditor-first, then optional disassembler, then Simulator (see
  comment block at `simulator-aarch64.h:1423-1428`).

`ConstructDecodeGraph()` at `decoder-aarch64.cc:66-88` builds the tree
from `kDecodeMapping` entries (defined in
`src/aarch64/decoder-constants-aarch64.h`) plus a synthetic
"unallocated" leaf, then calls `Compile()` on the `"Root"` node.

## 4. DecodeNode and CompiledDecodeNode

Two classes form the dispatch tree:

- `class DecodeNode` (`decoder-aarch64.h:528-800+`) — source
  representation: a name, a list of sampled bit positions, and a list
  of `(pattern, handler_name)` pairs. Each `DecodeNode` owns its
  `CompiledDecodeNode*` after compilation.
- `class CompiledDecodeNode` (`decoder-aarch64.h:453-526`) — runtime
  representation: a member-function pointer that extracts the
  next-level sampled bits from the `Instruction*`, plus a flat
  `decode_table_` array indexed by those bits, plus a back-pointer to
  the Decoder for leaf invocation.

The runtime call shape is at `decoder-aarch64.cc:1354-1367`:

```cpp
void CompiledDecodeNode::Decode(const Instruction* instr) const {
  if (IsLeafNode()) {
    // If this node is a leaf, call the registered visitor function.
    VIXL_ASSERT(decoder_ != NULL);
    decoder_->VisitNamedInstruction(instr, instruction_name_);
  } else {
    // Otherwise, using the sampled bit extractor for this node, look up the
    // next node in the decode tree, and call its Decode method.
    VIXL_ASSERT(bit_extract_fn_ != NULL);
    VIXL_ASSERT((instr->*bit_extract_fn_)() < decode_table_size_);
    VIXL_ASSERT(decode_table_[(instr->*bit_extract_fn_)()] != NULL);
    decode_table_[(instr->*bit_extract_fn_)()]->Decode(instr);
  }
}
```

Per non-leaf node the cost is one member-fn-ptr call to extract bits
plus one indexed pointer load plus one recursive call. The
`bit_extract_fn_` is a member function of `Instruction` selected at
compile-tree-construction time by `DecodeNode::GetBitExtractFunction`
(`decoder-aarch64.h:604-612`); under the hood these route to
`Instruction::Compress(mask)` which is the bitfield extractor at
`instructions-aarch64.h:255+`.

A typical AArch64 instruction reaches a leaf in 2-4 levels.

## 5. VisitNamedInstruction (the visitor fan-out)

`decoder-aarch64.cc:137-158`:

```cpp
void Decoder::VisitNamedInstruction(const Instruction* instr,
                                    const std::string& name) {
  std::list<DecoderVisitor*>::iterator it;
  Metadata m = {{"form", name}};
  uint32_t form_hash = Hash(name.c_str());

  // If an encoding is unallocated for this form, add the information to the
  // metadata.
  auto range = form_to_unalloc_.equal_range(form_hash);
  for (auto itu = range.first; itu != range.second; ++itu) {
    uint32_t mask = itu->second >> 32;
    uint32_t value = itu->second & 0xffffffff;
    if (instr->Mask(mask) == value) {
      m.insert({"unallocated", ""});
      break;
    }
  }

  for (it = visitors_.begin(); it != visitors_.end(); it++) {
    (*it)->Visit(&m, instr);
  }
}
```

This is the single hottest piece of the upstream path that the
predecode cache exists to eliminate. Per call it:

1. Constructs `Metadata m = {{"form", name}}` — a
   `std::unordered_map<std::string, std::string>` allocated on the
   heap. This is one heap allocation per instruction.
2. Hashes the form name once (`Hash(name.c_str())`).
3. Walks `form_to_unalloc_` to optionally annotate the metadata with an
   `"unallocated"` marker.
4. Iterates the `std::list<DecoderVisitor*>` and makes a virtual call
   per visitor. With the auditor + Simulator (no trace) this is two
   virtual calls; with trace enabled it is three.

## 6. Simulator::Visit (the form-hash → leaf lookup)

`simulator-aarch64.cc:2306-2323`:

```cpp
void Simulator::Visit(Metadata* metadata, const Instruction* instr) {
  VIXL_ASSERT(metadata->count("form") > 0);
  // Check for unallocated encodings.
  if (metadata->count("unallocated") > 0) {
    VisitUnallocated(instr);
    return;
  }

  std::string form = (*metadata)["form"];
  form_hash_ = Hash(form.c_str());
  const FormToVisitorFnMap* fv = Simulator::GetFormToVisitorFnMap();
  FormToVisitorFnMap::const_iterator it = fv->find(form_hash_);
  if (it == fv->end()) {
    VisitUnimplemented(instr);
  } else {
    (it->second)(this, instr);
  }
}
```

So the *Simulator's* visit, on top of the decoder's per-instruction
work, performs:

1. A second hash of the same form name (`Hash(form.c_str())`) and
   stores the result into the Simulator member `form_hash_`. Some
   leaves read `form_hash_` directly to choose between sub-behaviors —
   see "form_hash_ usage in leaves" below.
2. A lookup in `Simulator::GetFormToVisitorFnMap()` (the static map
   built once at construction; see `simulator-aarch64.cc:105`).
3. An indirect call through the function pointer stored in the map.

The map values are free functions or static thunks of type
`(Simulator*, const Instruction*) -> void` that dispatch into either a
plain `Simulator::VisitXXX` member or a `Simulator::Simulate_*` helper.

`VisitUnallocated` (cc:3915), `VisitUnimplemented` (cc:3907),
`VisitReserved` (cc:3896) cover the negative cases.

## 7. The visitor map header

`src/aarch64/decoder-visitor-map-aarch64.h` defines two macros that
populate the form-hash → visitor function lookup tables:

- `DEFAULT_FORM_TO_VISITOR_MAP(VISITORCLASS)` — entries like
  `{"dupm_z_i"_h, &VISITORCLASS::VisitSVEBroadcastBitmaskImm}`,
  `{"bfdot_asimdelem_e"_h, &VISITORCLASS::VisitUnimplemented}`. The
  trailing `_h` is a `consteval` user-defined literal that hashes the
  form name at compile time so the map keys are `uint32_t` not
  `std::string`.
- `SIM_AUD_VISITOR_MAP(VISITORCLASS)` — entries that route system
  instructions (e.g. `autia1716_hi_hints`) through `VisitSystem`.

`decoder-aarch64.h:41-44` and `:46+` define
`VISITOR_LIST_THAT_RETURN(V)` and `SIM_AUD_VISITOR_LIST_THAT_RETURN(V)`
— large `V(...)` macros enumerating every Visitor class member to
declare on the visitor base. They're consumed by `simulator-aarch64.h:1447-1454`
to declare every `VisitXXX` member function on `Simulator`.

## 8. The Instruction wrapper

`Instruction` (`src/aarch64/instructions-aarch64.h:228-327`) is a
zero-byte class (no members of its own); methods are called on an
`Instruction*` pointing directly at the 32-bit guest code word.
`GetInstructionBits()` (h:230):

```cpp
Instr GetInstructionBits() const {
  return *(reinterpret_cast<const Instr*>(this));
}
```

`Compress(mask)` (h:255) implements the Hacker's-Delight bit-compress
operation used for sampled-bit extraction. Field accessors
(`GetRd()`, `GetRn()`, `GetImmAddSub()`, `GetConditionBranch()`, etc.)
are generated by the `DEFINE_GETTER` macro (h:308-312). All accessors
recompute their bits from the raw word every time — there is no
caching at the `Instruction` layer.

`kInstructionSize = 4` (h:40). `GetNextInstruction()` returns the
following 32-bit instruction pointer.

## 9. State updated by leaves

A few representative leaves illustrate the patterns the cache must
preserve.

`VisitConditionalBranch` (`simulator-aarch64.cc:3946-3952`):

```cpp
void Simulator::VisitConditionalBranch(const Instruction* instr) {
  VIXL_ASSERT((form_hash_ == "b_only_condbranch"_h) ||
              (form_hash_ == "bc_only_condbranch"_h));
  if (ConditionPassed(instr->GetConditionBranch())) {
    WritePc(instr->GetImmPCOffsetTarget());
  }
}
```

Note the `form_hash_` assertion — see "form_hash_ usage in leaves"
below. `WritePc(target)` (`simulator-aarch64.h:1369-1374`) sets
`pc_ = AddressUntag(target)` and `pc_modified_ = true`, suppressing
auto-increment.

`VisitAddSubImmediate` (`simulator-aarch64.cc:4182-4186`):

```cpp
void Simulator::VisitAddSubImmediate(const Instruction* instr) {
  int64_t op2 = instr->GetImmAddSub()
                << ((instr->GetImmAddSubShift() == 1) ? 12 : 0);
  AddSubHelper(instr, op2);
}
```

`AddSubHelper` reads source registers off the instruction word, writes
the destination via `WriteRegister`, and updates NZCV when the S bit is
set.

`VisitLoadStoreUnsignedOffset` (`simulator-aarch64.cc:4346-4349`):

```cpp
void Simulator::VisitLoadStoreUnsignedOffset(const Instruction* instr) {
  int offset = instr->GetImmLSUnsigned() << instr->GetSizeLS();
  LoadStoreHelper(instr, offset, Offset);
}
```

Memory access goes through `MemRead<T>` / `MemWrite<T>` which call
into the `Memory` class (see
[`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md)).

## 10. form_hash_ usage in leaves

Many `Simulate_*` leaves are *shared* across multiple instruction
forms and switch on `form_hash_` to pick behavior. Example
(`simulator-aarch64.cc:2325-2344`):

```cpp
void Simulator::Simulate_PdT_PgZ_ZnT_ZmT(const Instruction* instr) {
  VectorFormat vform = instr->GetSVEVectorFormat();
  SimPRegister& pd = ReadPRegister(instr->GetPd());
  SimPRegister& pg = ReadPRegister(instr->GetPgLow8());
  SimVRegister& zm = ReadVRegister(instr->GetRm());
  SimVRegister& zn = ReadVRegister(instr->GetRn());

  switch (form_hash_) {
    case "match_p_p_zz"_h:
      match(vform, pd, zn, zm, /* negate_match = */ false);
      break;
    case "nmatch_p_p_zz"_h:
      match(vform, pd, zn, zm, /* negate_match = */ true);
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
  mov_zeroing(pd, pg, pd);
  PredTest(vform, pg, pd);
}
```

`form_hash_` is a Simulator data member set by `Simulator::Visit`
(see Step 6 above). **This is load-bearing for any cached path**:
calling a cached leaf without first restoring `form_hash_` may invoke
`VIXL_UNIMPLEMENTED()` or — worse — mis-execute when two forms route
to the same `Simulate_*` helper.

## 11. Per-instruction overhead summary

For an instruction that is *not* unallocated, with the typical
visitor configuration (auditor + Simulator, trace off), the upstream
path costs approximately:

| step | per-instruction cost |
|------|----------------------|
| BType check + alignment assert | ~0 in release |
| `decoder_->Decode(pc_)` | iterate visitor list to assert const, call root |
| `CompiledDecodeNode::Decode` walk | ~2-4 levels × (1 member-fn-ptr call + 1 indexed load) |
| `Decoder::VisitNamedInstruction` | 1 heap-allocated `unordered_map` build (`Metadata`), 1 string hash, 1 multimap range walk |
| visitor list iteration | virtual call per visitor (×2 typical, ×3 with trace) |
| `Simulator::Visit` | second string hash + 1 `unordered_map<uint32_t, FnPtr>::find` + 1 indirect call |
| `VisitXXX` / `Simulate_*` leaf | the actual semantics |
| `IncrementPc` + `UpdateBType` + `LogAllWrittenRegisters` (no-op when trace off) | trivial |
| auditor `InstructionIsAvailable()` assert | trivial after the auditor's per-instruction `Visit` |

The **two heap allocations** (the `Metadata` map and possibly the
`unordered_map::find` for the form lookup) plus **two string hashes
of the same form name** are the largest source of upstream
per-instruction overhead. They are exactly what the predecode cache
collapses.

## 12. What must be preserved if Decode is bypassed

Inputs to the [modification sketch](./gaby-vm-modification-sketch.md).
Any cached path that calls a leaf directly must still:

- **Pre-leaf:** set `pc_modified_ = false`; perform the BType /
  guarded-page check (or skip it on validated code); **set
  `form_hash_` to the cached form** (load-bearing — leaves read it).
- **Post-leaf:** assign `last_instr_ = ReadPc()`; call `IncrementPc()`
  so straight-line PC advance still happens; call
  `LogAllWrittenRegisters()` if trace is on; call `UpdateBType()`;
  satisfy the `cpu_features_auditor_.InstructionIsAvailable()` check
  (either by per-region pre-approval or by replaying the auditor's
  `Visit()`).
- **Branch handling:** if the leaf calls `WritePc()`, the loop's next
  iteration looks up the new `pc_`; nothing to do at the leaf
  boundary except not double-incrementing.
- **MOVPRFX continuation rule:** if the previous instruction was a
  MOVPRFX, the post-decode assertion `pc_->CanTakeSVEMovprfx(...)` runs.
  Cached path must preserve `last_instr_` and `form_hash_` between
  iterations.

The companion modification doc sketches how each of these is handled
in the Gaby-VM cache.

## 13. Reference: function-pointer cost vs branch

The leaf is reached today through:

1. ~2-4 member-function-pointer calls inside the decode tree.
2. 2-3 virtual calls on `DecoderVisitor::Visit`.
3. 1 indirect call on the form-to-visitor-fn-map result.

Total: ~5-8 indirect calls per instruction, plus the heap allocations
described above. A predecode cache that stores
`(form_hash, leaf_fn_ptr)` reduces this to:

1. 1 array lookup keyed on `pc_` (constant-time).
2. 1 indirect call on the cached `leaf_fn_ptr`.

That is the single largest dispatch optimization available without
changing instruction semantics. Subsequent optimizations (operand
pre-extraction, threaded interpretation, basic-block linking) build on
this baseline.

## Where to read next

- [`vixl-aarch64-simulator-architecture.md`](./vixl-aarch64-simulator-architecture.md)
  — the broader Simulator surface.
- [`gaby-vm-modification-sketch.md`](./gaby-vm-modification-sketch.md)
  — the cache design that consumes this dispatch model.
- [`vixl-extraction-map.md`](./vixl-extraction-map.md) — the import
  list including the decoder, instructions, and visitor-map files.
