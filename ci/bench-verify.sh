#!/usr/bin/env bash
# Copyright 2026, the gaby-vm authors
# SPDX-License-Identifier: BSD-3-Clause
#
# ci/bench-verify.sh [preset]   (default: dev-release)
#
# Run bench_business --verify: the bit-exact cache==decoder parity check over
# all five kernels. This is a deterministic gate — it exits non-zero on any
# mismatch or if the binary is missing. No timing involved.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/util.sh"

preset="${1:-dev-release}"
build_dir="$(ci_build_dir "$preset")"
bin="$(ci_find "$build_dir/bench" bench_business)"
[ -n "$bin" ] || bin="$(ci_find "$build_dir" bench_business)"

if [ -z "$bin" ]; then
  ci_log "bench_business not found under $build_dir (configure with -DGABY_VM_BUILD_BENCHMARKS=ON)"
  ci_summary "### ❌ Cache/decoder parity — bench_business missing"
  exit 1
fi

ci_log "verify: $bin --verify"
set +e
out="$("$bin" --verify 2>&1)"
status=$?
set -e
printf '%s\n' "$out"

verdict="$(printf '%s\n' "$out" | grep -E '^verify:' | tail -1)"
if [ "$status" -eq 0 ] && printf '%s' "$verdict" | grep -q 'OK'; then
  ci_summary "### ✅ Cache/decoder parity"
  ci_summary ""
  ci_summary "\`${verdict:-verify: OK}\`"
  exit 0
fi

ci_summary "### ❌ Cache/decoder parity MISMATCH"
ci_summary ""
ci_summary "\`\`\`"
ci_summary "$out"
ci_summary "\`\`\`"
exit 1
