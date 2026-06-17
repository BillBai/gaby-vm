# Dispatch-Flatten Profile and Design Analysis, 2026-06-11

[Chinese version](gaby-vm-dispatch-flatten-profile-2026-06-11.zh-cn.md)

This profile targets scalar business kernels in `bench_business`, specifically
`parse` and `fsm`, on the cache path. It breaks down the cost of the dispatch
hub, second dispatch inside leaves, virtual calls, MOVPRFX checks, operand
storage choices, and the possibility of removing `form_hash`.

The earlier 2026-05-27 hot-path profile covered `smoke` and `mixed`. This one
uses shapes closer to iOS business logic: branch-heavy parsing and a per-byte
state machine. The direction matches the earlier profile, but this note splits
the dispatch bucket more finely and measures MOVPRFX directly.

## Method

- `build/profile` used `RelWithDebInfo`: `-O2 -g -DNDEBUG` plus
  `-fno-omit-frame-pointer`. `-O2` gives cleaner function attribution than
  `-O3`; indirect PMF dispatch keeps leaves out of the driver loop either way.
- macOS `/usr/bin/sample`, 1 ms interval, 12 seconds per kernel, about 10.3k
  samples per kernel.
- The profiled build measured `parse` at 6.97 ns/instruction and `fsm` at
  6.67 ns/instruction, within 7 percent of the `-O3` headline numbers.
- Code state: commit `e670814`, after data-in-stream sentinel slots landed.

## Summary

- The dispatch hub is the largest single bucket: 38.6 percent for `parse` and
  42.1 percent for `fsm`. Most of that cost is fixed bookkeeping, not virtual
  dispatch itself: range checks, `form_hash_` writes, MOVPRFX checks, PC/BType
  updates, and last-instruction state.
- Literal "second dispatch plus virtual" cost is only about 5 to 10 percent.
  Flattening still matters because it collapses call layering and repeated
  operand extraction across layers.
- MOVPRFX checking measured at about 6 percent. The cost is not two integer
  comparisons; it is the read/write dependency around `form_hash_` plus a bool
  live across the leaf call.
- Do not store every operand in the cache entry. A full operand payload costs
  16 to 32 extra bytes per entry. Specialize and inline handlers instead, and
  store only expensive derived values such as logical-immediate bitmasks.
- If flattening covers both scalar `Visit*` leaves and NEON/SVE `Simulate_*`
  leaves, the hot path can drop `form_hash` entirely. The 32-bit slot recovered
  from the existing 16-byte entry can hold rare expensive derived operands.

## Self-Time Buckets

Flat self-time view from `sample`, by top of stack. Percentages use each
kernel's total samples, about 10238 for `parse` and 10250 for `fsm`.

| Bucket | Meaning | parse | fsm |
|--------|---------|------:|----:|
| **dispatch hub**: `ExecuteInstructionCached`, `RunFrom`, `GabyHookedWritePc` | Fixed per-instruction work: cache range checks, `form_hash_` write, MOVPRFX comparisons, indirect call, `IncrementPc`, `UpdateBType`, `last_instr` bookkeeping | **38.6%** | **42.1%** |
| **Visit\* entry layer** | Leaf entry, including mask dispatch and some operand extraction | 9.1% | 16.3% |
| **Shared helpers**: `AddSubHelper`, `LoadStoreHelper`, `LogicalHelper` | Operand extraction, mask switch, register reads/writes | 35.4% | 22.0% |
| **Operand derivation helpers**: `IsLoad`, `CalcLSDataSize`, `GetImmPCOffsetTarget`, `DecodeImmBitMask` | Pure field decoding | 6.1% | 5.4% |
| **Actual semantics**: `AddWithCarry`, `ConditionPassed` | Irreducible operations | 8.7% | 13.6% |
| Unattributed stub frames | Unknown | ~2.1% | ~0.6% |

The hottest single function in both kernels was `ExecuteInstructionCached`,
followed by `AddSubHelper`. In `fsm`, `VisitConditionalBranch` was third. That
leaf has no second dispatch; its cost is operand extraction plus branch
condition and PC-write calls that did not inline.

## Findings

### 1. Flattening Is About Structure

`AddSubHelper` contains one `Mask(AddSubOpMask)` switch. Most of its cost comes
from repeated operand extraction and register access, not from the switch alone.
The PMF call `(this->*pmf)(pc_)` also sits inside the dispatch hub, but the
target tends to be stable per PC and predicted well.

The direct cost of "second dispatch plus virtual call" is about 5 to 10 percent.
The real opportunity is structural collapse:

- remove `ExecuteInstructionCached -> Visit* -> Helper -> semantic helper`
  call layering;
- avoid extracting the same fields in both `Visit*` and the shared helper;
- specialize each form into its own inline handler.

### 2. Dispatch Hub Cost Is Bookkeeping

`ExecuteInstructionCached` currently does range checks, a BTI gate, MOVPRFX
comparisons, a `form_hash_` write, the indirect leaf call, and end-of-instruction
bookkeeping. The `form_hash_` write is redundant for scalar `Visit*` leaves; it
exists for NEON/SVE `Simulate_*` leaves that switch on the form hash.

### 3. MOVPRFX Costs About 6 Percent

The two MOVPRFX hash values are `constexpr uint32_t` constants, so the check
looks cheap. Ablation replaced the computed `last_instr_was_movprfx` with
`false`, rebuilt, and ran each kernel three times:

| Kernel | Baseline | Ablated | Speedup |
|--------|----------|---------|--------:|
| parse | 7.19 ns/insn, range 7.17-7.28 | 6.73, range 6.62-6.78 | **6.4%** |
| fsm | 6.79 ns/insn, range 6.78-6.92 | 6.38, range 6.36-6.40 | **6.0%** |

The cost comes from second-order effects:

```cpp
bool last_instr_was_movprfx = (form_hash_ == C1) || (form_hash_ == C2);
form_hash_ = entry->form_hash;
(this->*pmf)(pc_);
if (last_instr_was_movprfx) { ... }
```

`form_hash_` creates a read/write/read dependency chain, and
`last_instr_was_movprfx` must remain live across the leaf call. A real fix, such
as gating with a predecode flag or tracking the previous entry explicitly, will
not get the full ablation win but should recover most of the cost.

### 4. Operand Storage Is the Wrong Default

Storing a full operand payload would add 16 to 32 bytes to every cache entry,
which increases D-cache footprint on streamed code. For business kernels, most
operand extraction is a handful of inline `ubfx`/`and` operations; the expensive
part is the call structure and repeated extraction across layers.

The better default is specialize plus inline. Keep extraction in code, but let
the compiler schedule it in a straight-line handler. Store only expensive
derived values, such as logical-immediate bitmasks from `DecodeImmBitMask`.

### 5. Full Flattening Can Drop `form_hash`

Hot-path `form_hash_` readers are concentrated in `Simulate_*` and SVE-family
leaves:

| Reader class | Count | Hot path? | Blocks removal? |
|--------------|------:|-----------|-----------------|
| `switch (form_hash_)` in `Simulate_*` / `SimulateSVE*` | 82 | yes | yes |
| `form_hash_ ==` in those bodies plus MOVPRFX checks | 42 | yes | yes |
| `CanTakeSVEMovprfx(form_hash_)` | 2 | yes | should be flag-gated |
| Disassembler lookup | 4 | no | no |
| CPU features auditor lookup | 2 | no | no |
| Assertions | about 4 | no in NDEBUG | no |

Flattening scalar `Visit*` leaves helps business kernels, but `form_hash_` must
remain for NEON/SVE entries. Flattening both scalar and `Simulate_*` families
removes all execution-track readers, so the dispatch hub no longer needs the
per-instruction write.

The current entry layout is `{uint32_t form_hash; uint32_t flags; const void*
leaf;}`, still 16 bytes because of pointer alignment. Removing `form_hash`
therefore frees 32 bits without growing the entry. That slot is a good home for
rare expensive derived operands.

## Milestone-1 Shape

This is a working memo, not committed scope:

1. Flatten second dispatch into specialized inline handlers. Scalar `Visit*`
   leaves are mandatory; `Simulate_*` scope is a scheduling decision.
2. Collapse the `Visit* -> Helper` call layer and repeated cross-layer operand
   extraction.
3. Flag-gate MOVPRFX. If both families flatten, remove the `form_hash_` write.
4. Reserve the recovered 32 bits for expensive derived operands such as
   `DecodeImmBitMask`.

Expected impact: if flattening specializes and inlines the whole leaf chain, a
realistic win is about 25 to 35 percent total time, or 1.3x to 1.5x on business
kernels. This step alone will not reach 50x; it is a structural base for later
work.

## Reproduction

```bash
cmake -S . -B build/profile -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DGABY_VM_BUILD_BENCHMARKS=ON -DGABY_VM_BUILD_NATIVE_BASELINE=ON \
  -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer"
cmake --build build/profile --target bench_business

./build/profile/bench/bench_business --kernel parse --mode cache --seconds 30 & \
  BPID=$!; sleep 1; sample $BPID 12 1 -file /tmp/parse_sample.txt -mayDie; kill $BPID
```

For the MOVPRFX ablation, temporarily change the `last_instr_was_movprfx`
calculation in `simulator-aarch64.h` to `false`, rebuild `bench_business`, and
compare `ns_per_instruction` across three runs.

## Related Docs

- [`gaby-vm-business-bench-2026-06-08.md`](./gaby-vm-business-bench-2026-06-08.md):
  first business benchmark data and method.
- [`gaby-vm-cache-hotpath-profile-2026-05-27.md`](./gaby-vm-cache-hotpath-profile-2026-05-27.md):
  smoke/mixed cache hot-path profile.
- [`gaby-vm-predecode-cache-design.md`](./gaby-vm-predecode-cache-design.md):
  authoritative predecode/dispatch cache design.
