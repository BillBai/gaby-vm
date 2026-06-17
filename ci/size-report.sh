#!/usr/bin/env bash
# Copyright 2026, the gaby-vm authors
# SPDX-License-Identifier: BSD-3-Clause
#
# ci/size-report.sh [--base <sizes.json>] [--preset <preset>] [--out <sizes.json>]
#
# Measure two binary-size numbers and report them with deltas:
#   * lib_a_bytes        — raw libgaby_vm.a (release, unstripped archive)
#   * probe_stripped_bytes — size_probe linked + stripped, the footprint an
#                            embedder actually pays for the public API surface
# Writes a sizes.json and a markdown table. REPORT-ONLY: always exits 0 on the
# size itself (only a missing library is an error). When --base is given, adds a
# delta column versus that baseline; otherwise notes that no base is available.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/util.sh"

preset="dev-release"
base=""
out="sizes.json"
while [ $# -gt 0 ]; do
  case "$1" in
    --base)   base="$2";   shift 2 ;;
    --preset) preset="$2"; shift 2 ;;
    --out)    out="$2";    shift 2 ;;
    *) ci_log "unknown arg: $1"; shift ;;
  esac
done

build_dir="$(ci_build_dir "$preset")"
lib="$(ci_find "$build_dir" libgaby_vm.a)"
probe="$(ci_find "$build_dir/bench" size_probe)"
[ -n "$probe" ] || probe="$(ci_find "$build_dir" size_probe)"

if [ -z "$lib" ]; then
  ci_log "libgaby_vm.a not found under $build_dir (run ci/build.sh $preset first)"
  exit 1
fi

lib_bytes="$(ci_file_bytes "$lib")"

# Stripped embedder footprint: strip a copy of size_probe and take its file
# size. The constant binary overhead cancels in the head-vs-base delta, so the
# delta isolates the change in what the public API pulls in.
probe_bytes=0
if [ -n "$probe" ]; then
  tmp="$(mktemp)"
  cp "$probe" "$tmp"
  strip "$tmp" 2>/dev/null || strip -x "$tmp" 2>/dev/null || true
  probe_bytes="$(ci_file_bytes "$tmp")"
  rm -f "$tmp"
else
  ci_log "size_probe not found (build bench with -DGABY_VM_BUILD_BENCHMARKS=ON); reporting lib only"
fi

printf '{ "lib_a_bytes": %s, "probe_stripped_bytes": %s }\n' "$lib_bytes" "$probe_bytes" >"$out"
ci_log "wrote $out (lib=$lib_bytes probe=$probe_bytes)"

# Delta column.
delta() { # <cur> <base> -> "+1.2 KB" / "-0.3 KB" / "0"
  awk -v c="$1" -v b="$2" 'BEGIN {
    d = c - b
    if (d == 0) { print "0"; exit }
    printf "%s%.1f KB", (d > 0 ? "+" : "-"), (d < 0 ? -d : d) / 1024
  }'
}

have_base=0
base_lib=0
base_probe=0
if [ -n "$base" ] && [ -f "$base" ]; then
  have_base=1
  base_lib="$(grep -o '"lib_a_bytes"[^0-9]*[0-9]*' "$base" | grep -o '[0-9]*$' || echo 0)"
  base_probe="$(grep -o '"probe_stripped_bytes"[^0-9]*[0-9]*' "$base" | grep -o '[0-9]*$' || echo 0)"
  : "${base_lib:=0}"
  : "${base_probe:=0}"
fi

ci_summary "### 📦 Binary size"
ci_summary ""
if [ "$have_base" -eq 1 ]; then
  ci_summary "| artifact | size | Δ vs base |"
  ci_summary "| --- | ---: | ---: |"
  ci_summary "| libgaby_vm.a (raw) | $(ci_human_kb "$lib_bytes") | $(delta "$lib_bytes" "$base_lib") |"
  ci_summary "| size_probe (stripped) | $(ci_human_kb "$probe_bytes") | $(delta "$probe_bytes" "$base_probe") |"
else
  ci_summary "| artifact | size |"
  ci_summary "| --- | ---: |"
  ci_summary "| libgaby_vm.a (raw) | $(ci_human_kb "$lib_bytes") |"
  ci_summary "| size_probe (stripped) | $(ci_human_kb "$probe_bytes") |"
  ci_summary ""
  ci_summary "_No base size available to compare against._"
fi

exit 0
