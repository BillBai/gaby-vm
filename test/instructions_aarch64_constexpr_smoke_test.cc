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

#include <cstdint>

#include "aarch64/instructions-aarch64.h"

namespace {

using vixl::aarch64::IsSVEFormat;
using vixl::aarch64::IsVectorFormat;
using vixl::aarch64::LaneCountFromFormat;
using vixl::aarch64::LaneSizeInBitsFromFormat;
using vixl::aarch64::LaneSizeInBytesFromFormat;
using vixl::aarch64::LaneSizeInBytesLog2FromFormat;
using vixl::aarch64::MaxIntFromFormat;
using vixl::aarch64::MaxLaneCountFromFormat;
using vixl::aarch64::MaxUintFromFormat;
using vixl::aarch64::MinIntFromFormat;
using vixl::aarch64::RegisterSizeInBitsFromFormat;
using vixl::aarch64::RegisterSizeInBytesFromFormat;
using vixl::aarch64::ScalarFormatFromFormat;
using vixl::aarch64::ScalarFormatFromLaneSize;

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

// The eight helpers added by neon-clearforwrite-and-helpers-inline. Each
// static_assert below proves the corresponding helper is reachable as
// constexpr from this TU. Failure to compile = promotion broke. The cases
// also indirectly exercise GetUintMask's constexpr promotion (via
// Max{Int,Uint}).

// ScalarFormatFromLaneSize: lane bits -> scalar format enum.
static_assert(ScalarFormatFromLaneSize(8) == vixl::aarch64::kFormatB);
static_assert(ScalarFormatFromLaneSize(16) == vixl::aarch64::kFormatH);
static_assert(ScalarFormatFromLaneSize(32) == vixl::aarch64::kFormatS);
static_assert(ScalarFormatFromLaneSize(64) == vixl::aarch64::kFormatD);

// LaneSizeInBytesLog2FromFormat: log2 of lane byte width.
static_assert(LaneSizeInBytesLog2FromFormat(vixl::aarch64::kFormat16B) == 0);
static_assert(LaneSizeInBytesLog2FromFormat(vixl::aarch64::kFormat8H) == 1);
static_assert(LaneSizeInBytesLog2FromFormat(vixl::aarch64::kFormat4S) == 2);
static_assert(LaneSizeInBytesLog2FromFormat(vixl::aarch64::kFormat2D) == 3);
static_assert(LaneSizeInBytesLog2FromFormat(vixl::aarch64::kFormatVnQ) == 4);

// MaxLaneCountFromFormat: max ASIMD lane count by lane size.
static_assert(MaxLaneCountFromFormat(vixl::aarch64::kFormat16B) == 16);
static_assert(MaxLaneCountFromFormat(vixl::aarch64::kFormat8H) == 8);
static_assert(MaxLaneCountFromFormat(vixl::aarch64::kFormat4S) == 4);
static_assert(MaxLaneCountFromFormat(vixl::aarch64::kFormat2D) == 2);

// IsVectorFormat: false on scalar B/H/S/D, true on vector formats.
static_assert(!IsVectorFormat(vixl::aarch64::kFormatB));
static_assert(!IsVectorFormat(vixl::aarch64::kFormatH));
static_assert(!IsVectorFormat(vixl::aarch64::kFormatS));
static_assert(!IsVectorFormat(vixl::aarch64::kFormatD));
static_assert(IsVectorFormat(vixl::aarch64::kFormat4S));
static_assert(IsVectorFormat(vixl::aarch64::kFormat2D));
static_assert(IsVectorFormat(vixl::aarch64::kFormatVnD));

// ScalarFormatFromFormat: maps a vector format to its scalar peer. Compiles
// iff both ScalarFormatFromLaneSize and LaneSizeInBitsFromFormat (Lever A)
// are constexpr-reachable.
static_assert(ScalarFormatFromFormat(vixl::aarch64::kFormat4S) ==
              vixl::aarch64::kFormatS);
static_assert(ScalarFormatFromFormat(vixl::aarch64::kFormat2D) ==
              vixl::aarch64::kFormatD);
static_assert(ScalarFormatFromFormat(vixl::aarch64::kFormat16B) ==
              vixl::aarch64::kFormatB);

// MaxIntFromFormat / MinIntFromFormat / MaxUintFromFormat: integer extents
// per lane width. These exercise GetUintMask's constexpr promotion
// (src/utils-vixl.h); failure to compile = the closure broke.
static_assert(MaxIntFromFormat(vixl::aarch64::kFormat4S) == INT32_MAX);
static_assert(MaxIntFromFormat(vixl::aarch64::kFormat2D) == INT64_MAX);
static_assert(MaxIntFromFormat(vixl::aarch64::kFormat8H) == INT16_MAX);

static_assert(MinIntFromFormat(vixl::aarch64::kFormat4S) == INT32_MIN);
static_assert(MinIntFromFormat(vixl::aarch64::kFormat2D) == INT64_MIN);

static_assert(MaxUintFromFormat(vixl::aarch64::kFormat4S) == UINT32_MAX);
static_assert(MaxUintFromFormat(vixl::aarch64::kFormat2D) == UINT64_MAX);
static_assert(MaxUintFromFormat(vixl::aarch64::kFormat16B) == UINT8_MAX);

}  // namespace

int main() { return 0; }
