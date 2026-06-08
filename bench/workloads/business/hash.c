// K2 `hash` — integer-dense compute microkernel.
//
// Models the "compute a hash / checksum / digest over a buffer" shape that is
// pervasive in app business logic (dedup keys, integrity checks, hash maps,
// bloom filters). It is the interpreter's *best case*: a tight loop of integer
// ALU (multiply / shift / xor), almost no branches, fully register-resident.
// It anchors the optimistic end of the slowdown spread — the scalar ILP ceiling
// a no-JIT interpreter can reach when dispatch and operand decode are the only
// taxes left.
//
// Self-contained contract (shared by every business microkernel; see
// bench/workloads/business/README in the design doc):
//   * one exported `uint64_t bench_<name>_main(void)`; result returned in x0,
//     used only as a correctness oracle (decoder == cache == committed value),
//     never on the timed path.
//   * NO external calls, NO .rodata references (no global tables, no jump
//     tables): every byte the kernel touches is synthesized in-register from an
//     LCG, so the extracted .text is a single relocation-free function callable
//     directly both inside the simulator and as the native baseline.
//   * deterministic: same seed -> same path -> same result every pass.
//
// Regeneration: see bench/workloads/business/gen_business_workloads.sh.

#include <stdint.h>

// Tuned so one pass executes tens of thousands of dynamic instructions: enough
// that the harness per-pass clock read (runner.cc reads steady_clock once per
// RunFrom) and the LR re-arm are negligible, matching the mixed workload's
// ~64k/pass rather than smoke's noisy 32/pass.
#define HASH_ROUNDS 16
#define HASH_BYTES_PER_ROUND 256

uint64_t bench_hash_main(void) {
  // splitmix64-style LCG state synthesizes the input stream in-register, so no
  // memory buffer (and therefore no memset/memcpy libcall) is needed.
  uint64_t lcg = 0x9E3779B97F4A7C15ull;
  uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a 64-bit offset basis.

  for (int round = 0; round < HASH_ROUNDS; ++round) {
    for (int i = 0; i < HASH_BYTES_PER_ROUND; ++i) {
      // Advance the LCG and take a byte off the top.
      lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
      uint64_t b = (lcg >> 56) & 0xff;

      // FNV-1a step.
      h ^= b;
      h *= 0x00000100000001b3ull;  // FNV-1a 64-bit prime.

      // MurmurHash3 finalizer-style avalanche, to keep the ALU pipe busy with
      // the multiply/shift/xor mix that real digest code is made of.
      h ^= h >> 33;
      h *= 0xff51afd7ed558ccdull;
      h ^= h >> 33;
    }
  }
  return h;
}
