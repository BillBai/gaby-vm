# Adding FP/NEON to `bench_business`: `applogic`, 2026-06-09

[Chinese version](gaby-vm-business-bench-applogic-2026-06-09.zh-cn.md)

This note records the fifth `bench_business` kernel, `applogic`. The first four
kernels were scalar-integer only because they used `-mgeneral-regs-only`.
`applogic` adds a mixed workload: normal business logic with about 9 percent
scalar double FP and a small amount of NEON.

The motivation is iOS UI code. Layout, geometry, animation, text, and transform
code routinely use scalar floating point through `CGFloat`, `CGRect`, affine
transforms, and related APIs. A pure-integer benchmark understates that common
shape.

## Summary

- `applogic` walks a batch of UI-like elements, performs integer business work,
  does layout-style scalar double geometry, and applies a small four-lane NEON
  rectangle transform.
- Measured dynamic mix: FP 6.7 percent plus NEON 1.9 percent, or 8.6 percent
  combined.
- It is the only `bench_business` kernel with FP/NEON and the only one not built
  with `-mgeneral-regs-only`.

Results on an M4 Pro, `dev-release`, two runs averaged:

| Kernel | Shape | native ns/insn | cache ns/insn | slowdown | cache/decoder |
|--------|-------|----------------:|---------------:|---------:|--------------:|
| hash | Integer dependency chain | 0.310 | 5.79 | ~19x | ~14x |
| fsm | Branch-heavy | 0.067 | 6.18 | ~92x | ~14x |
| parse | Decoder | 0.060 | 6.59 | ~110x | ~13x |
| struct | ILP-friendly memory | 0.035 | 7.24 | ~210x | ~13x |
| **applogic** | **Mixed, with FP/NEON** | **0.030** | **10.02** | **~330x** | **~10x** |

The important finding is that FP/NEON raises gaby-vm's flat scalar line. The
first four integer kernels sit near 6.5 ns/instruction. Adding only 8.6 percent
FP/NEON raises `applogic` to about 10 ns/instruction, roughly 55 percent higher.
VIXL's FP/NEON leaves cost more than integer leaves because they handle NaNs,
rounding modes, wider operands, and lane movement.

## 1. Kernel Shape and Self-Containment

The kernel generates a batch of elements on the stack with an LCG, then iterates
48 times. Each element runs integer work first: a four-way branch on kind,
flag-driven xor/rotate operations, a field writeback, and checksum updates.

About one in eight elements then runs CGFloat-style geometry:

- convert width/height to `double` with `scvtf`;
- apply a simple affine transform;
- clamp with min/max;
- compute area and diagonal with `fsqrt`;
- divide and quantize back to integer with `fcvtzu`.

Every eight elements also run a four-lane NEON rectangle transform. The final
`x0` value is the correctness oracle.

The hard part was preserving the zero-relocation self-contained contract after
allowing FP/NEON:

- Floating constants often fall into literal pools. The kernel avoids
  non-encodable double constants. Values either use AArch64-encodable FP
  immediates or come from integer LCG state through `scvtf`.
- `sqrt` defaults to a libcall when the compiler needs `errno` behavior.
  `-fno-math-errno` lets it compile to a single `fsqrt`, which fits the
  freestanding benchmark.
- The kernel does not use `arm_neon.h`. It uses compiler vector extensions and
  `__builtin_convertvector`, with encodable splat constants.

The generation script now uses per-kernel flags. The first four kernels keep
`-mgeneral-regs-only`; `applogic` uses `-fno-math-errno` and records its tag as
`flags=O2/fp+neon/no-jump-tables`.

## 2. Correctness Gate

`bench_business --verify` single-steps `applogic` in cache mode and decoder
mode. Dynamic count and `x0` match bit-for-bit:

```text
dynamic=101532
x0=0x00000414d717d726
```

All five business kernels now pass `--verify`. The dynamic instruction mix was
measured by combining a per-address hit histogram from single-step execution
with static disassembly classification. Its sum matches the verified dynamic
count.

## 3. Findings

### 3.1 The Flat Line Applies Only to Scalar Integer Code

The earlier note's "cache mode is flat at about 6.5 ns/instruction" conclusion
is accurate for scalar integer workloads. `applogic` shows that real FP raises
the line. With 8.6 percent FP/NEON, cache cost rises to about
10 ns/instruction. Real business code lands between the scalar-integer line and
the heavier FP/NEON line depending on FP mix.

### 3.2 FP Business Logic Is the New Worst Case

`applogic` slows down by about 330x, worse than `struct` at about 210x. Native is
very fast on this shape, roughly the same class as `struct`, and gaby-vm spends
more per FP/NEON instruction than per integer instruction. Together those two
facts make layout-style FP business logic the hardest measured shape so far.

### 3.3 Optimization Implications

Dispatch and operand-predecode work still help `applogic` because it pays the
same fixed per-instruction tax. But it also exposes FP/NEON leaf cost that the
integer kernels cannot show. If gaby-vm needs FP-heavy business logic near 50x,
the project may also need to inspect FP/NEON leaf paths after dispatch work.

## 4. Limits

- Single machine, no pinning, variance in the tens of percent.
- The FP/NEON ratio was chosen to represent "mostly business logic plus some
  layout FP", not captured from a specific app trace.
- NEON is only about 1.9 percent, matching the expectation that ordinary iOS
  business code vectorizes only a little. Apple Silicon has no SVE hardware, so
  this note does not involve SVE.
- This is still a native-denominator benchmark. A Lua/JSC LLInt head-to-head is
  still needed for interpreter comparisons.

## 5. Index

- First four-kernel business benchmark:
  [`gaby-vm-business-bench-2026-06-08.md`](./gaby-vm-business-bench-2026-06-08.md).
- Kernel source: `bench/workloads/business/applogic.c`.
- Generation script: `bench/workloads/business/gen_business_workloads.sh`.
- Harness documentation: [`bench/README.md`](../../bench/README.md).
