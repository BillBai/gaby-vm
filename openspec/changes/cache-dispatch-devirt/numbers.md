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

| after | parse | hash | struct | fsm | applogic | notes |
|-------|------:|-----:|-------:|----:|---------:|-------|
| C1 (devirt) | | | | | | text-size delta: |
