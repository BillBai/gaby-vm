# cache-dispatch-devirt — measured numbers

Host: Apple M1 Pro, macOS 26.5.1, AC power, idle during timing.
`bench_business --mode cache --seconds 1.0`, medians of 3, ns/insn.
Before-reference (branch tip = cache-hotpath-tier1 T5 row):
parse 8.455 / hash 7.177 / struct 9.384 / fsm 8.944 / applogic 10.416.

## Task 1.1 — branch-tip applogic profile (GO gate)

Profile build (`build/profile`, `-O3 -g -fno-omit-frame-pointer`),
`sample` 12s @1ms on `--kernel applogic --mode cache`, 2026-07-03,
~10600 non-dyld samples:

| bucket | share |
|---|---:|
| `ExecuteInstructionCached` (hub) | 23.6% |
| `gaby_vm::Simulator::RunFrom` | 2.8% |
| AddSubHelper / LogicalHelper / LoadStore[Pair]Helper | ~20% |
| `DecodeImmBitMask` | 5.9% |
| Visit* entry layer (CondSelect/CondBranch/LogicalImm/…) | ~15% |
| `GetImmPCOffsetTarget` + `ConditionPassed` | ~3.6% |
| FP/NEON residue (fcvts/scvtf/ClearForWrite/FPIntegerConvert/…) | ~5% |

Dispatch hub share 26.4% ≥ the ~20% abort threshold → **GO** (synthesis C0
condition satisfied). Raw dump kept out of tree (session scratchpad); the
bucket table above is the on-record figure the C1–C7 predictions key on.

## Per-task results

`bench_business --mode cache --seconds 1.0`, medians of 3, ns/insn. Host as
above. All shapes improved vs the T5 before-row; no shape regressed.

| after | parse | hash | struct | fsm | applogic | notes |
|-------|------:|-----:|-------:|----:|---------:|-------|
| T5 (before) | 8.455 | 7.177 | 9.384 | 8.944 | 10.416 | branch tip 16df2bc |
| C1 (devirt) | 8.116 | 6.654 | 8.648 | 8.057 | 9.918 | text-size delta below |
| delta | -4.0% | -7.3% | -7.8% | -9.9% | -4.8% | all faster |

Raw 3-run samples (ns/insn):

| run | parse | hash | struct | fsm | applogic |
|-----|------:|-----:|-------:|----:|---------:|
| 1 | 8.116 | 6.548 | 8.648 | 8.057 | 10.137 |
| 2 | 7.957 | 6.675 | 8.666 | 8.118 | 9.918 |
| 3 | 8.247 | 6.654 | 8.570 | 7.948 | 9.807 |

Acceptance (design D6 / tasks 3.2): ≥ neutral overall — met decisively (every
shape faster). applogic 9.918 lands just above the ~9.4-9.8 prediction; the ABI
was the point, and it is in place. Gates: `bench_business --verify` OK
(cache==decoder bit-identical on all five kernels), `ctest -R vixl_port` 3/3,
full debug `ctest` 24/24 (data-in-stream + movprfx abort tests included).

### Text-size delta

`size` on the dev-release `bench_business`, A/B stash of the same commit:

| metric | before (16df2bc) | after (C1) | delta |
|--------|-----------------:|-----------:|------:|
| bench_business `__text` | 1,405,228 B | 1,486,568 B | +81,340 B (+5.79%) |
| bench_business `__TEXT` seg | 1,622,016 B | 1,703,936 B | +81,920 B (+5.05%) |
| predecode_cache.cc.o `__TEXT` | 5,682 B | 125,617 B | +119,935 B |

The ~460 stamped thunks (Visit* + Simulate*) are all emitted into
`predecode_cache.cc.o` (the TU that ODR-uses `ResolvePredecodeHandler`'s
pmf->thunk table). The +81 KB in the final binary is the accepted I-cache
growth for C1 (design Risks); only a handful of thunks are hot per workload, and
specialized handlers replace the hottest ones in C2+.

### Verification gate — static binding of the thunk leaf call

Release object `predecode_cache.cc.o`, `objdump -dr`. Every stamped thunk shares
one prologue + one direct leaf `bl` + one epilogue; the `GABY_UNLIKELY` branches
(BTI gate, MOVPRFX post-check, trace gate) are outlined to `.cold` partitions.

| thunk | leaf `bl` reloc | kind |
|-------|-----------------|------|
| `GabyThunk_VisitSystem` | `ARM64_RELOC_BRANCH26 -> Simulator::VisitSystem` | virtual leaf, direct-bound |
| `GabyThunk_VisitConditionalBranch` | `ARM64_RELOC_BRANCH26 -> Simulator::VisitConditionalBranch` | virtual leaf, direct-bound |
| `GabyThunk_SimulateFPConvert` | `ARM64_RELOC_BRANCH26 -> Simulator::SimulateFPConvert` | non-virtual leaf, direct |

Zero `blr` and zero vtable loads inside any thunk body — the qualified call
`sim->Simulator::VisitXxx(sim->pc_)` devirtualizes statically as designed. The
dispatch side (`ExecuteInstructionCached` inlined into `gaby_vm::Simulator::
RunFrom`) is `ldr x8, [entry, #8]` (handler-slot load) then `blr x8` — one
dependent load, one indirect call, no pmf load (spec scenario 1).

## Task 3.1 — side-by-side audit: old hub vs thunk body

Pre-C1 `ExecuteInstructionCached` (old hub) vs post-C1
`ExecuteInstructionCached` + `GabyThunkPrologue` + qualified leaf +
`GabyThunkEpilogue`. Steps in execution order; `[U]` = `GABY_UNLIKELY`.

| # | old hub step | post-C1 location | equivalent? |
|---|--------------|------------------|-------------|
| 1 | `VIXL_ASSERT(IsWordAligned(pc_))` | ExecuteInstructionCached | identical |
| 2 | `VIXL_ASSERT(cache_ != nullptr)` | ExecuteInstructionCached | identical |
| 3 | `pc_modified_ = false` | GabyThunkPrologue (1st stmt) | moved after entry resolution; nothing between old spot and new spot reads pc_modified_ -> identical |
| 4 | pc->entry resolution: cur_range_ fast path `[U]` + `FindRange` cold + `GabyAbortPcNotInRange` `[U]` | ExecuteInstructionCached | identical (unchanged block) |
| 5 | BTI gate `if (entry->flags & 1) {...}` `[U]` | GabyThunkPrologue | identical body, same `[U]` hint |
| 6 | MOVPRFX latch: `local = gaby_prev_was_movprfx_; gaby_prev_was_movprfx_ = flags&2` | GabyThunkPrologue | identical |
| 7 | `form_hash_ = entry->form_hash` (before leaf) | GabyThunkPrologue (last stmt, before leaf) | identical, still pre-leaf |
| 8 | leaf: load pmf; `(this->*pmf)(pc_)` **virtual dispatch** | thunk body: `sim->Simulator::VisitXxx(sim->pc_)` **direct bl** | same target member, same arg `pc_`; devirtualized (only intended change) |
| 9 | post-leaf MOVPRFX check `if (local) {ASSERT; CHECK(CanTakeSVEMovprfx)}` `[U]` | GabyThunkEpilogue | identical body, same `[U]` |
| 10 | `last_instr_ = ReadPc()` | GabyThunkEpilogue | identical |
| 11 | `IncrementPc()` | GabyThunkEpilogue | identical |
| 12 | trace gate `if (GetTraceParameters()!=0) LogAllWrittenRegisters()` `[U]` | GabyThunkEpilogue | identical, same `[U]` |
| 13 | BType gate `if (btype_!=Default \|\| next_btype_!=Default) UpdateBType()` | GabyThunkEpilogue | identical |
| — | (implicit fallthrough) | thunk `return nullptr` (C1 reserved value) | new; ignored by caller -> no behavior change |

Only two intended deltas: (3) `pc_modified_` reset relocated after entry
resolution (provably observation-equivalent), and (8) the virtual pmf call
replaced by a statically-bound qualified call to the same member. Confirmed by
the differential + absolute oracles across `vixl_port` (3/3) and the
cache==decoder `--verify` bit-check on all five business kernels.
