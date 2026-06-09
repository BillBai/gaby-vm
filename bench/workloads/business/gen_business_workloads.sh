#!/bin/sh
# Regenerate the committed business-logic microkernel workload headers.
#
# Each kernel is a single self-contained C function (bench/workloads/business/
# <name>.c) compiled to relocation-free AArch64 .text, then emitted as a
# constexpr uint32_t[] header (mirrors the smoke workload's llvm-mc pipeline,
# but the source is compiled C instead of hand-written asm — see the design doc
# at docs/superpowers/specs/ for the rationale).
#
# The compile flags enforce the self-contained contract:
#   --target=aarch64-linux-gnu  ELF output, .text named the standard way
#   -O2                          representative optimized business-logic codegen
#   -mgeneral-regs-only          NO NEON/FP (matches the iOS business-logic scope)
#   -fno-jump-tables             switches -> compare/branch chains, no .rodata
#   -fno-builtin -ffreestanding  no implicit memcpy/memset libcalls
#   -fno-asynchronous-unwind-tables  no .eh_frame noise
#   -ffixed-x18                  never allocate x18 (CRITICAL, see below)
#
# Why -ffixed-x18 (do not remove): --target=aarch64-linux-gnu treats x18 as a
# general scratch register, but Apple platforms (macOS AND iOS) RESERVE x18 as
# the platform register. native_business executes these bytes directly in the
# host process, so a kernel-clobbered x18 would make the native denominator
# ABI-dependent; and more fundamentally, real iOS business-logic codegen never
# touches x18 (the Apple toolchain reserves it), so a workload that did would not
# be representative of the iOS hot-fix scenario. -ffixed-x18 keeps the ELF
# extraction pipeline (linux triple -> .text) while matching the Apple ABI's
# x18 reservation. Without it, the `parse` kernel allocated x18 14 times.
#
# This script emits ONLY the machine-derived header fields (bytes, static word
# count, tag). The per-kernel dynamic instruction count and expected-x0 oracle
# live in the hand-maintained oracle_data.h, which this script never touches.
# After regenerating, rebuild bench_business and run `bench_business --verify`;
# if it reports a changed dynamic count or expected x0, update oracle_data.h.
#
# Usage:  sh bench/workloads/business/gen_business_workloads.sh
set -eu

LLVM="${LLVM:-/opt/homebrew/opt/llvm/bin}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CLANG="$LLVM/clang"
OBJCOPY="$LLVM/llvm-objcopy"
OBJDUMP="$LLVM/llvm-objdump"

# Shared flags for every kernel (the self-containment contract).
CFLAGS_COMMON="--target=aarch64-linux-gnu -O2 -fno-jump-tables \
        -fno-builtin -ffreestanding -fno-asynchronous-unwind-tables -ffixed-x18"

# The first four kernels are pure scalar integer, so they add -mgeneral-regs-only
# to hard-guarantee NO NEON/FP. `applogic` is the representative MIXED kernel: it
# deliberately keeps FP + NEON (real iOS business logic touches layout/geometry,
# which is scalar double FP, plus a little simd NEON), so it drops
# -mgeneral-regs-only and adds -fno-math-errno. The latter is load-bearing:
# without it __builtin_sqrt lowers to a `bl sqrt` libcall — an external
# relocation that fails the self-containment gate below — instead of a single
# `fsqrt` instruction. Every FP/NEON constant in applogic.c is fmov-encodable, so
# nothing lands in a .rodata literal pool (which would also be a relocation).
CFLAGS_SCALAR="-mgeneral-regs-only"
CFLAGS_MIXED="-fno-math-errno"

KERNELS="parse hash struct fsm applogic"

CLANG_VERSION="$("$CLANG" --version | head -1 | sed 's/ *$//')"

for k in $KERNELS; do
  src="$HERE/$k.c"
  obj="/tmp/business_$k.o"
  bin="/tmp/business_$k.bin"
  hdr="$HERE/${k}_workload_data.h"

  echo "=== $k ==="
  # Per-kernel flags + a flag-provenance string for the generator tag.
  case "$k" in
    applogic)
      kflags="$CFLAGS_COMMON $CFLAGS_MIXED"
      flagtag="O2/fp+neon/no-jump-tables"
      ;;
    *)
      kflags="$CFLAGS_COMMON $CFLAGS_SCALAR"
      flagtag="O2/general-regs-only/no-jump-tables"
      ;;
  esac
  # shellcheck disable=SC2086
  "$CLANG" $kflags -c "$src" -o "$obj"

  # Self-containment gate: any relocation means an external/global reference
  # (a libcall, a .rodata table) that would not survive .text extraction.
  reloc="$("$OBJDUMP" -r "$obj" | grep -E 'R_AARCH64|RELOCATION' || true)"
  if [ -n "$reloc" ]; then
    echo "FATAL: $k has relocations (not self-contained):" >&2
    echo "$reloc" >&2
    exit 1
  fi

  "$OBJCOPY" -O binary --only-section=.text "$obj" "$bin"

  sha="$(shasum -a 256 "$src" | cut -c1-12)"
  words="$(( $(wc -c < "$bin") / 4 ))"

  # Emit the header: ONLY the machine-derived artifacts (byte array, static word
  # count, generator tag). The runtime-captured oracle constants (dynamic count,
  # expected x0) deliberately live in the hand-maintained oracle_data.h, which
  # this script never touches — that separation keeps regeneration free of a
  # source-of-truth fight (a regen here can never silently reset the oracle).
  cap="$(echo "$k" | awk '{print toupper(substr($0,1,1)) substr($0,2)}')"
  {
    printf '#ifndef GABY_VM_BENCH_WORKLOADS_BUSINESS_%s_WORKLOAD_DATA_H_\n' \
      "$(echo "$k" | tr '[:lower:]' '[:upper:]')"
    printf '#define GABY_VM_BENCH_WORKLOADS_BUSINESS_%s_WORKLOAD_DATA_H_\n\n' \
      "$(echo "$k" | tr '[:lower:]' '[:upper:]')"
    printf '#include <cstddef>\n#include <cstdint>\n\n'
    printf '// Business-logic microkernel "%s" — committed bytes, regenerated\n' "$k"
    printf '// offline by bench/workloads/business/gen_business_workloads.sh from\n'
    printf '// bench/workloads/business/%s.c. Do not edit by hand. The dynamic\n' "$k"
    printf '// instruction count and expected-x0 oracle live in oracle_data.h.\n\n'
    printf 'namespace gaby_vm_bench {\n\n'
    printf 'inline constexpr std::uint32_t k%sWorkloadInstructions[] = {\n' "$cap"
    # 4 little-endian words per line.
    od -An -tx4 -v "$bin" | awk '{for(i=1;i<=NF;i++) printf "    0x%s,\n", $i}'
    printf '};\n\n'
    printf 'inline constexpr std::size_t k%sWorkloadStaticWordCount =\n' "$cap"
    printf '    sizeof(k%sWorkloadInstructions) / sizeof(k%sWorkloadInstructions[0]);\n\n' "$cap" "$cap"
    printf 'inline constexpr char k%sWorkloadGeneratorTag[] =\n' "$cap"
    printf '    "%s; flags=%s; source_sha256=%s";\n\n' \
      "$CLANG_VERSION" "$flagtag" "$sha"
    printf '}  // namespace gaby_vm_bench\n\n'
    printf '#endif\n'
  } > "$hdr"

  echo "  -> $words words, sha=$sha, header written"
done

echo "done. Now rebuild bench_business and run --verify; if any dynamic count or"
echo "expected x0 changed, update bench/workloads/business/oracle_data.h by hand."
