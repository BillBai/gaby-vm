// Smoke benchmark workload — straight-line ALU instructions wrapped in
// the same LR-based exit convention BenchCodeGenerator uses upstream.
// The body has no branches, so the dynamic instruction count equals the
// static word count and the simulator's existing kEndOfSimAddress sentinel
// terminates RunFrom at the trailing RET. See
// openspec/changes/baseline-benchmark-harness/design.md decisions 1, 2.
//
// Regeneration (see bench/README.md for the long form):
//
//   LLVM=/opt/homebrew/opt/llvm/bin
//   $LLVM/llvm-mc -triple=aarch64-linux-gnu -filetype=obj \
//       bench/workloads/smoke_workload.s -o /tmp/smoke.o
//   $LLVM/llvm-objcopy -O binary --only-section=.text \
//       /tmp/smoke.o /tmp/smoke.bin
//
// `-triple=aarch64-linux-gnu` forces ELF output regardless of host so the
// section is named `.text` (Mach-O names it `__text`); the resulting
// /tmp/smoke.bin is exactly 128 bytes (32 AArch64 instructions).
//
// Then convert /tmp/smoke.bin into uint32_t literals and write
// bench/workloads/smoke_workload_data.h with the four constants
// kSmokeWorkloadInstructions, kSmokeWorkloadStaticWordCount,
// kSmokeWorkloadDynamicInstructionsPerIteration, kSmokeWorkloadGeneratorTag.
//
// Total instruction count: 32 (2 prologue + 28 ALU body + 2 epilogue).
// Dynamic == static because the body is branch-free.

.text
.globl smoke_main
smoke_main:
    // Prologue (2 instructions): save frame and link register.
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    // Body (28 instructions): 7 repetitions of a 4-instruction ALU pattern.
    add     x0,  x0,  #1
    eor     x1,  x1,  x2
    orr     x3,  x3,  x4
    sub     x5,  x5,  #1

    add     x0,  x0,  #1
    eor     x1,  x1,  x2
    orr     x3,  x3,  x4
    sub     x5,  x5,  #1

    add     x0,  x0,  #1
    eor     x1,  x1,  x2
    orr     x3,  x3,  x4
    sub     x5,  x5,  #1

    add     x0,  x0,  #1
    eor     x1,  x1,  x2
    orr     x3,  x3,  x4
    sub     x5,  x5,  #1

    add     x0,  x0,  #1
    eor     x1,  x1,  x2
    orr     x3,  x3,  x4
    sub     x5,  x5,  #1

    add     x0,  x0,  #1
    eor     x1,  x1,  x2
    orr     x3,  x3,  x4
    sub     x5,  x5,  #1

    add     x0,  x0,  #1
    eor     x1,  x1,  x2
    orr     x3,  x3,  x4
    sub     x5,  x5,  #1

    // Epilogue (2 instructions): restore frame and return.
    ldp     x29, x30, [sp], #16
    ret
