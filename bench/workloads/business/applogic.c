// K5 `applogic` — representative mixed app-logic microkernel.
//
// The first four kernels are compiled -mgeneral-regs-only (pure scalar
// integer), which understates real iOS business logic: anything touching
// layout/geometry is scalar double FP (CGFloat == double on 64-bit), and simd /
// Core Animation / Accelerate paths emit a little NEON. This kernel models a
// "lay out and process a list of UI elements" pass: the bulk (~90%) is the
// ordinary integer business logic the other kernels exercise (field load/store,
// branchy per-element dispatch, an integer digest), with a slice of scalar
// double FP for the CGFloat-style geometry (affine transform, min/max clamp,
// area, diagonal) and a small NEON part (a 4-lane packed-rect transform), so
// the dynamic instruction mix reflects real app code (~10% FP+NEON) instead of
// pretending business logic is FP-free.
//
// It is the ONLY kernel compiled WITHOUT -mgeneral-regs-only. To keep the
// extracted .text relocation-free (the self-containment gate), it must emit NO
// literal-pool constant: AArch64 `fmov` encodes only a few immediates, so any
// other double constant would become an adrp+ldr from .rodata (a relocation).
// Every FP value here is therefore either built from the integer LCG via scvtf
// or uses an fmov-encodable constant (1.0/1.5/2.0/0.5/1.25); the clamp uses two
// runtime values (fminnm/fmaxnm), never a constant bound. No libm (no sin/cos):
// only the hardware fsqrt (which needs -fno-math-errno to stay an instruction
// rather than a `bl sqrt` libcall). NEON uses the compiler's vector_size
// extension + __builtin_convertvector (no arm_neon.h, which -ffreestanding
// would not give us). The double/NEON results are folded back into x0 as an
// integer oracle.
//
// Self-contained contract otherwise as in hash.c: no external calls, no
// .rodata, stack-only working set, deterministic (same seed -> same path ->
// same x0).
//
// Regeneration: see bench/workloads/business/gen_business_workloads.sh.

#include <stdint.h>

#define APP_COUNT 64
#define APP_ROUNDS 48

typedef struct {
  uint32_t id;
  uint32_t kind;
  uint32_t w;
  uint32_t h;
  uint32_t flags;
} Element;

// Compiler vector extension — guarantees NEON codegen without arm_neon.h, which
// -ffreestanding does not provide. A 4-wide float/int vector maps to a single
// Q register; the ops below become fmul/fadd/scvtf/fcvtzs on .4s lanes.
typedef float float4 __attribute__((vector_size(16)));
typedef int32_t int4 __attribute__((vector_size(16)));

uint64_t bench_applogic_main(void) {
  Element els[APP_COUNT];

  // Synthesize a deterministic element list on the stack (explicit field
  // stores, no memset/memcpy libcall).
  uint64_t lcg = 0x14057B7EF767814Full;
  for (int i = 0; i < APP_COUNT; ++i) {
    lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
    els[i].id = (uint32_t)i;
    els[i].kind = (uint32_t)(lcg >> 30) & 0x3;
    els[i].w = ((uint32_t)(lcg >> 16) & 0x1ff) + 1;  // 1..512 px
    els[i].h = ((uint32_t)(lcg >> 40) & 0x1ff) + 1;
    els[i].flags = (uint32_t)(lcg >> 8) & 0x7;
  }

  uint64_t checksum = 0;

  for (int round = 0; round < APP_ROUNDS; ++round) {
    for (int i = 0; i < APP_COUNT; ++i) {
      Element* e = &els[i];
      uint32_t w = e->w;
      uint32_t h = e->h;

      // --- integer business logic (the ~90%) --------------------------------
      // Branchy per-element dispatch + integer mix + a written-back field, the
      // shape the parse/struct/fsm kernels share.
      uint32_t v;
      switch (e->kind) {
        case 0:
          v = (w * 3u + h) ^ e->id;
          break;
        case 1:
          v = (w > h) ? (w - h) : (h - w);
          break;
        case 2:
          v = (w ^ h) + (e->flags << 4);
          break;
        default:
          v = (w + h) * 0x9E3779B1u;
          break;
      }
      if (e->flags & 0x1) {
        v ^= 0x5BD1E995u;
      }
      if (e->flags & 0x2) {
        v = (v << 7) | (v >> 25);
      }
      e->w = (v & 0x1ff) + 1;  // store back; feeds the next round's geometry.
      checksum += v;

      // --- scalar double FP: CGFloat-style layout geometry (~7% dynamic) -----
      // Gated to ~1/8 of elements (real code does not recompute every view's
      // frame every pass). Constants are fmov-encodable only; all other
      // magnitudes come from the data via scvtf.
      if ((e->flags & 0x7) == 0) {
        double dw = (double)w;                   // scvtf
        double dh = (double)h;                   // scvtf
        double fw = dw * 1.5 + (double)e->id;    // affine: scale 1.5 + tx
        double fh = dh * 2.0 - (double)e->kind;  // affine: scale 2.0 - ty
        double mn = (fw < fh) ? fw : fh;         // fminnm (runtime operands)
        double mx = (fw > fh) ? fw : fh;         // fmaxnm
        double area = fw * fh;                   // fmul
        double diag = __builtin_sqrt(fw * fw + fh * fh);  // fsqrt
        double ratio = mx / (mn + 1.0);                   // fadd (1.0) + fdiv
        // Quantize the geometry back into the integer oracle (fcvtzu).
        checksum += (uint64_t)(area) + (uint64_t)(diag) + (uint64_t)(ratio);
      }

      // --- NEON: a 4-lane packed-rect transform (~2% dynamic) ---------------
      // Models the simd_float4 / CGAffineTransform path: load four coordinates,
      // scale+translate them in one vector op, fold back. Gated to every 8th
      // element. Splat constants (1.25f/0.5f) are fmov-encodable, so no vector
      // literal pool is needed.
      if ((i & 0x7) == 0) {
        int4 iv = {(int32_t)els[i & (APP_COUNT - 1)].w,
                   (int32_t)els[(i + 1) & (APP_COUNT - 1)].h,
                   (int32_t)els[(i + 2) & (APP_COUNT - 1)].w,
                   (int32_t)els[(i + 3) & (APP_COUNT - 1)].h};
        float4 fv = __builtin_convertvector(iv, float4);  // scvtf.4s
        fv = fv * 1.25f + 0.5f;                           // fmul.4s + fadd.4s
        int4 ov = __builtin_convertvector(fv, int4);      // fcvtzs.4s
        checksum += (uint32_t)ov[0] + (uint32_t)ov[1] + (uint32_t)ov[2] +
                    (uint32_t)ov[3];
      }
    }
  }

  return checksum;
}
