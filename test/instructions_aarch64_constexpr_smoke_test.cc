// Compile-time fold smoke test for the six VectorFormat helpers promoted to
// constexpr inline by the openspec change
// `neon-format-helpers-constexpr-inline`. A successful build proves the
// promotion is real: the static_asserts below are evaluated at compile time,
// which is only possible if `IsSVEFormat`, `LaneSizeInBitsFromFormat`,
// `LaneSizeInBytesFromFormat`, `LaneCountFromFormat`,
// `RegisterSizeInBitsFromFormat`, and `RegisterSizeInBytesFromFormat` are
// reachable as constexpr from a TU that includes
// `aarch64/instructions-aarch64.h`. If a future maintenance change moves any
// of them back to `.cc` (or drops the `constexpr` qualifier), this TU stops
// compiling — which is the intended guardrail.
//
// Runtime body is trivial: the proof lives in the static_asserts.

#include "aarch64/instructions-aarch64.h"

namespace {

using vixl::aarch64::IsSVEFormat;
using vixl::aarch64::LaneCountFromFormat;
using vixl::aarch64::LaneSizeInBitsFromFormat;
using vixl::aarch64::LaneSizeInBytesFromFormat;
using vixl::aarch64::RegisterSizeInBitsFromFormat;
using vixl::aarch64::RegisterSizeInBytesFromFormat;

// IsSVEFormat: SVE formats true, ASIMD / scalar false.
static_assert(IsSVEFormat(vixl::aarch64::kFormatVnB));
static_assert(IsSVEFormat(vixl::aarch64::kFormatVnD));
static_assert(IsSVEFormat(vixl::aarch64::kFormatVnQ));
static_assert(!IsSVEFormat(vixl::aarch64::kFormat16B));
static_assert(!IsSVEFormat(vixl::aarch64::kFormat4S));
static_assert(!IsSVEFormat(vixl::aarch64::kFormatS));

// LaneSizeInBitsFromFormat: maps lane width to 8/16/32/64/128/256.
static_assert(LaneSizeInBitsFromFormat(vixl::aarch64::kFormat16B) == 8);
static_assert(LaneSizeInBitsFromFormat(vixl::aarch64::kFormat8H) == 16);
static_assert(LaneSizeInBitsFromFormat(vixl::aarch64::kFormat4S) == 32);
static_assert(LaneSizeInBitsFromFormat(vixl::aarch64::kFormat2D) == 64);
static_assert(LaneSizeInBitsFromFormat(vixl::aarch64::kFormat1Q) == 128);
static_assert(LaneSizeInBitsFromFormat(vixl::aarch64::kFormatVnO) == 256);
static_assert(LaneSizeInBitsFromFormat(vixl::aarch64::kFormatS) == 32);

// LaneSizeInBytesFromFormat: trampoline over LaneSizeInBitsFromFormat / 8.
static_assert(LaneSizeInBytesFromFormat(vixl::aarch64::kFormat4S) == 4);
static_assert(LaneSizeInBytesFromFormat(vixl::aarch64::kFormat2D) == 8);
static_assert(LaneSizeInBytesFromFormat(vixl::aarch64::kFormat16B) == 1);

// LaneCountFromFormat: count of lanes by format.
static_assert(LaneCountFromFormat(vixl::aarch64::kFormat16B) == 16);
static_assert(LaneCountFromFormat(vixl::aarch64::kFormat8B) == 8);
static_assert(LaneCountFromFormat(vixl::aarch64::kFormat4S) == 4);
static_assert(LaneCountFromFormat(vixl::aarch64::kFormat2D) == 2);
static_assert(LaneCountFromFormat(vixl::aarch64::kFormatS) == 1);

// RegisterSizeInBitsFromFormat: scalar/ASIMD only (asserts !IsSVEFormat),
// folded via the inline IsSVEFormat above.
static_assert(RegisterSizeInBitsFromFormat(vixl::aarch64::kFormatS) ==
              vixl::aarch64::kSRegSize);
static_assert(RegisterSizeInBitsFromFormat(vixl::aarch64::kFormatD) ==
              vixl::aarch64::kDRegSize);
static_assert(RegisterSizeInBitsFromFormat(vixl::aarch64::kFormat16B) ==
              vixl::aarch64::kQRegSize);

// RegisterSizeInBytesFromFormat: trampoline over RegisterSizeInBitsFromFormat
// / 8. Compiles iff the whole dependency closure is constexpr-reachable.
static_assert(RegisterSizeInBytesFromFormat(vixl::aarch64::kFormatS) ==
              vixl::aarch64::kSRegSize / 8);
static_assert(RegisterSizeInBytesFromFormat(vixl::aarch64::kFormat2D) ==
              vixl::aarch64::kQRegSize / 8);

}  // namespace

int main() { return 0; }
