// K3 `struct` — record/array transform microkernel.
//
// Models the "iterate a list of model objects, read fields, compute a derived
// value, write some back" shape: the bread-and-butter of app business logic
// (mapping DTOs, recomputing totals, filtering/annotating records). Its
// signature cost is offset-based load/store against a struct array plus a
// moderate amount of data-dependent branching — it exercises the load/store
// leaf path that the mixed cache profile measured at ~16%, on a realistic
// scalar access pattern rather than NEON bulk moves.
//
// Self-contained contract: see hash.c header. The record array lives on the
// stack (well under the 12 KB simulator stack floor), is filled by an explicit
// store loop (no memset libcall), and is transformed in place. Result in x0 is
// an oracle only.
//
// Regeneration: see bench/workloads/business/gen_business_workloads.sh.

#include <stdint.h>

#define STRUCT_COUNT 128
#define STRUCT_ROUNDS 24

typedef struct {
  uint32_t id;
  uint32_t a;
  uint32_t b;
  uint32_t flags;
} Record;

uint64_t bench_struct_main(void) {
  Record recs[STRUCT_COUNT];

  // Populate the array deterministically with an LCG (explicit field stores,
  // not a bulk initializer).
  uint64_t lcg = 0xB5297A4D1B3C5E79ull;
  for (int i = 0; i < STRUCT_COUNT; ++i) {
    lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
    recs[i].id = (uint32_t)i;
    recs[i].a = (uint32_t)(lcg >> 32);
    recs[i].b = (uint32_t)lcg;
    recs[i].flags = (uint32_t)(lcg >> 28) & 0x7;
  }

  uint64_t total = 0;

  for (int round = 0; round < STRUCT_ROUNDS; ++round) {
    for (int i = 0; i < STRUCT_COUNT; ++i) {
      Record* r = &recs[i];
      uint32_t a = r->a;
      uint32_t b = r->b;

      // Branchy per-record derivation — the conditionals real transform code
      // carries (feature flags, validity gates, clamps).
      uint32_t derived;
      if (r->flags & 0x1) {
        derived = a + b * 3u;
      } else if (r->flags & 0x2) {
        derived = (a > b) ? (a - b) : (b - a);
      } else {
        derived = (a ^ b) + r->id;
      }

      if (r->flags & 0x4) {
        derived ^= 0x5BD1E995u;
      }

      // Write a derived field back (the store half of the load/store pattern)
      // and feed the next record so the chain has a real dependency.
      r->a = derived;
      total += derived;
    }
  }

  return total;
}
