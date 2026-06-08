// K1 `parse` — serialized-record parser + validator microkernel.
//
// Models the "decode a wire payload" shape that dominates app business logic:
// pulling fields out of a protobuf/TLV-style byte stream, switching on a
// wire-type tag, reading variable-length integers, and bounds-checking every
// step. It is deliberately branch-dense and data-dependent: the field tags and
// lengths come out of a pseudo-random stream, so the wire-type switch and the
// varint continuation loop mispredict the way a real decoder does on real
// traffic. This is the workload that most directly exercises the single
// polymorphic-`blr` dispatch ceiling discussed in the cache hot-path profile.
//
// Self-contained contract: see hash.c header. No external calls, no .rodata
// (the input buffer is synthesized on the stack from an LCG; the wire-type
// switch is compiled to a compare/branch chain via -fno-jump-tables, so there
// is no jump table in .rodata). Result returned in x0 is an oracle only.
//
// Regeneration: see bench/workloads/business/gen_business_workloads.sh.

#include <stdint.h>

#define PARSE_BUF_BYTES 512
#define PARSE_ROUNDS 48

// Protobuf wire types (lower 3 bits of a field tag).
enum {
  WIRE_VARINT = 0,
  WIRE_I64 = 1,
  WIRE_LEN = 2,
  WIRE_I32 = 5,
};

uint64_t bench_parse_main(void) {
  uint8_t buf[PARSE_BUF_BYTES];

  // Synthesize a deterministic pseudo-random payload on the stack, byte by byte
  // (an explicit store loop rather than a bulk initializer, so no memset/memcpy
  // libcall is emitted and the bytes look like real wire traffic).
  uint64_t lcg = 0x243F6A8885A308D3ull;
  for (int i = 0; i < PARSE_BUF_BYTES; ++i) {
    lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
    buf[i] = (uint8_t)(lcg >> 56);
  }

  uint64_t checksum = 0;

  for (int round = 0; round < PARSE_ROUNDS; ++round) {
    uint32_t pos = 0;
    // Walk the buffer as a sequence of (tag, value) records until it runs out.
    while (pos < PARSE_BUF_BYTES) {
      uint8_t tag = buf[pos++];
      uint32_t field = tag >> 3;
      uint32_t wire = tag & 0x7;

      if (wire == WIRE_VARINT) {
        // LEB128 varint: keep reading while the continuation bit is set.
        uint64_t value = 0;
        uint32_t shift = 0;
        while (pos < PARSE_BUF_BYTES) {
          uint8_t b = buf[pos++];
          value |= (uint64_t)(b & 0x7f) << shift;
          shift += 7;
          if ((b & 0x80) == 0 || shift >= 64) {
            break;
          }
        }
        checksum += value ^ (field * 0x9E3779B1u);
      } else if (wire == WIRE_LEN) {
        // Length-delimited: a one-byte length, then that many payload bytes,
        // bounds-checked against the buffer end (the validation a real decoder
        // must do before trusting an attacker-controlled length).
        uint32_t len = buf[pos < PARSE_BUF_BYTES ? pos : PARSE_BUF_BYTES - 1];
        ++pos;
        if (pos + len > PARSE_BUF_BYTES) {
          len = PARSE_BUF_BYTES - pos;  // clamp; resync at the boundary.
        }
        uint64_t acc = 0;
        for (uint32_t k = 0; k < len; ++k) {
          acc = (acc << 1) ^ buf[pos + k];
        }
        pos += len;
        checksum += acc + field;
      } else if (wire == WIRE_I32) {
        if (pos + 4 <= PARSE_BUF_BYTES) {
          uint32_t v = (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
                       ((uint32_t)buf[pos + 2] << 16) |
                       ((uint32_t)buf[pos + 3] << 24);
          checksum ^= v;
          pos += 4;
        } else {
          pos = PARSE_BUF_BYTES;
        }
      } else if (wire == WIRE_I64) {
        if (pos + 8 <= PARSE_BUF_BYTES) {
          uint64_t v = 0;
          for (int j = 0; j < 8; ++j) {
            v |= (uint64_t)buf[pos + j] << (8 * j);
          }
          checksum += v;
          pos += 8;
        } else {
          pos = PARSE_BUF_BYTES;
        }
      } else {
        // Unknown wire type (3, 4, 6, 7): a malformed record. Skip one byte and
        // resync — the error path real parsers take.
        checksum ^= 0xABCD;
      }
    }
  }

  return checksum;
}
