// K4 `fsm` — branch-dense state machine / scanner microkernel.
//
// Models the "scan a string/byte stream through a state machine" shape:
// tokenizers, lexical validators, UTF-8 / format checkers, simple matchers.
// Its defining cost is *unpredictable* control flow — the next state depends on
// the current state and the current byte's class, and both come out of a
// pseudo-random stream, so the per-byte dispatch mispredicts hard. It is the
// interpreter's *worst case* and the most direct argument for threaded dispatch
// (per-form branch-prediction history), anchoring the pessimistic end of the
// slowdown spread.
//
// Self-contained contract: see hash.c header. The scan buffer is synthesized on
// the stack from an LCG; character classes are computed arithmetically (range
// compares), not via a .rodata lookup table; the state switch is a compare
// chain (-fno-jump-tables). Result in x0 is an oracle only.
//
// Regeneration: see bench/workloads/business/gen_business_workloads.sh.

#include <stdint.h>

#define FSM_BUF_BYTES 1024
#define FSM_ROUNDS 24

enum {
  ST_DEFAULT = 0,
  ST_WORD = 1,
  ST_NUMBER = 2,
  ST_STRING = 3,
  ST_ESCAPE = 4,
};

// Arithmetic character classification — no lookup table, so nothing lands in
// .rodata. Branches here are part of the point: they are the per-byte decisions
// a real scanner makes.
static inline uint32_t classify(uint8_t c) {
  if (c >= 'a' && c <= 'z') {
    return 1;  // lower
  }
  if (c >= 'A' && c <= 'Z') {
    return 2;  // upper
  }
  if (c >= '0' && c <= '9') {
    return 3;  // digit
  }
  if (c == '"') {
    return 4;  // quote
  }
  if (c == '\\') {
    return 5;  // backslash
  }
  if (c == ' ' || c == '\t' || c == '\n') {
    return 6;  // space
  }
  return 0;  // other
}

uint64_t bench_fsm_main(void) {
  uint8_t buf[FSM_BUF_BYTES];

  uint64_t lcg = 0xD1B54A32D192ED03ull;
  for (int i = 0; i < FSM_BUF_BYTES; ++i) {
    lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
    // Bias toward printable ASCII so the scanner spends time in real states
    // rather than mostly in ST_DEFAULT.
    buf[i] = (uint8_t)(0x20 + ((lcg >> 56) % 0x5f));
  }

  uint64_t tokens = 0;
  uint64_t digest = 0;

  for (int round = 0; round < FSM_ROUNDS; ++round) {
    uint32_t state = ST_DEFAULT;
    for (int i = 0; i < FSM_BUF_BYTES; ++i) {
      uint8_t c = buf[i];
      uint32_t cls = classify(c);

      switch (state) {
        case ST_DEFAULT:
          if (cls == 1 || cls == 2) {
            state = ST_WORD;
            ++tokens;
          } else if (cls == 3) {
            state = ST_NUMBER;
            ++tokens;
          } else if (cls == 4) {
            state = ST_STRING;
            ++tokens;
          }
          break;
        case ST_WORD:
          if (cls == 0 || cls == 6) {
            state = ST_DEFAULT;
          } else if (cls == 4) {
            state = ST_STRING;
            ++tokens;
          }
          digest = digest * 31 + c;
          break;
        case ST_NUMBER:
          if (cls != 3) {
            state = ST_DEFAULT;
          } else {
            digest += c - '0';
          }
          break;
        case ST_STRING:
          if (cls == 5) {
            state = ST_ESCAPE;
          } else if (cls == 4) {
            state = ST_DEFAULT;  // closing quote.
          } else {
            digest ^= (digest << 5) + c;
          }
          break;
        case ST_ESCAPE:
          state = ST_STRING;  // consume one escaped byte.
          digest += c;
          break;
        default:
          state = ST_DEFAULT;
          break;
      }
    }
  }

  return tokens ^ (digest * 0x100000001b3ull);
}
