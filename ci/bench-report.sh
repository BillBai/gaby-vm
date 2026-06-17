#!/usr/bin/env bash
# Copyright 2026, the gaby-vm authors
# SPDX-License-Identifier: BSD-3-Clause
#
# ci/bench-report.sh [--seconds <s>] [--preset <preset>] [--out <bench.json>]
#
# Drive the WHOLE benchmark harness and emit a markdown three-way report plus a
# bench.json moving-baseline artifact. REPORT-ONLY: it exits non-zero only if a
# required binary is missing or a run crashes — never on a timing value.
#
# Coverage ("utilize all we have"):
#   * bench_business / native_business — five microkernels, three-way
#     (native / decoder / cache)
#   * bench_baseline / native_baseline — the mixed VIXL workload
#   * bench_smoke — pipeline sanity check (exit status only)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/util.sh"

seconds="1.0"
preset="dev-release"
out="bench.json"
while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) seconds="$2"; shift 2 ;;
    --preset)  preset="$2";  shift 2 ;;
    --out)     out="$2";     shift 2 ;;
    *) ci_log "unknown arg: $1"; shift ;;
  esac
done

bench_dir="$(ci_build_dir "$preset")/bench"
[ -d "$bench_dir" ] || { ci_log "bench dir missing: $bench_dir (build with -DGABY_VM_BUILD_BENCHMARKS=ON)"; exit 1; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Run a bench binary and emit "shape throughput_insn_per_sec" lines.
extract() { "$@" 2>/dev/null | awk '/^workload:/{w=$2} /^throughput_insn_per_sec:/{print w, $2}'; }

bb="$(ci_find "$bench_dir" bench_business)"
[ -n "$bb" ] || { ci_log "bench_business missing — nothing to report"; exit 1; }

: >"$tmp/native"; : >"$tmp/decoder"; : >"$tmp/cache"

ci_log "business three-way (${seconds}s/shape)"
extract "$bb" --mode decoder --seconds "$seconds" >>"$tmp/decoder"
extract "$bb" --mode cache   --seconds "$seconds" >>"$tmp/cache"
nb="$(ci_find "$bench_dir" native_business)"
[ -n "$nb" ] && extract "$nb" --seconds "$seconds" >>"$tmp/native" || ci_log "native_business absent (build -DGABY_VM_BUILD_NATIVE_BASELINE=ON for the native column)"

bl="$(ci_find "$bench_dir" bench_baseline)"
if [ -n "$bl" ]; then
  ci_log "mixed VIXL workload"
  extract "$bl" --mode decoder --seconds "$seconds" >>"$tmp/decoder"
  extract "$bl" --mode cache   --seconds "$seconds" >>"$tmp/cache"
  nbl="$(ci_find "$bench_dir" native_baseline)"
  [ -n "$nbl" ] && { extract "$nbl" --seconds "$seconds" >>"$tmp/native" || ci_log "native_baseline run failed (non-fatal)"; }
fi

sm="$(ci_find "$bench_dir" bench_smoke)"
smoke="skipped"
if [ -n "$sm" ]; then
  if "$sm" --mode cache --seconds 0.1 >/dev/null 2>&1; then smoke="ok"; else smoke="FAILED"; fi
fi

# Merge the three files into a markdown table + JSON. Order follows decoder
# (always populated). BSD awk has no ARGIND, so key on FILENAME.
awk '
  FILENAME ~ /\/native$/  { nat[$1]=$2; next }
  FILENAME ~ /\/cache$/   { cac[$1]=$2; next }
  FILENAME ~ /\/decoder$/ { dec[$1]=$2; if(!seen[$1]){seen[$1]=1; ord[++n]=$1} next }
  END {
    print "| shape | native (M/s) | decoder (M/s) | cache (M/s) | cache/decoder | slowdown vs native |" > "'"$tmp/table.md"'"
    print "| --- | ---: | ---: | ---: | ---: | ---: |" >> "'"$tmp/table.md"'"
    print "{ \"seconds\": " ('"$seconds"'+0) ", \"shapes\": [" > "'"$out"'"
    for (i=1;i<=n;i++) {
      s=ord[i]; d=dec[s]+0; c=cac[s]+0; na=nat[s]+0
      dm=sprintf("%.1f", d/1e6); cm=sprintf("%.1f", c/1e6)
      nm=(na>0)?sprintf("%.0f", na/1e6):"—"
      cd=(d>0)?sprintf("%.1fx", c/d):"—"
      sd=(na>0 && c>0)?sprintf("%.0fx", na/c):"—"
      printf "| %s | %s | %s | %s | %s | %s |\n", s, nm, dm, cm, cd, sd >> "'"$tmp/table.md"'"
      printf "%s{ \"shape\": \"%s\", \"native\": %d, \"decoder\": %d, \"cache\": %d }", (i>1?",\n":""), s, na, d, c >> "'"$out"'"
    }
    print "\n] }" >> "'"$out"'"
  }
' "$tmp/native" "$tmp/decoder" "$tmp/cache"

ci_log "wrote $out"
ci_summary "## 📊 Benchmark report — \`$preset\`, ${seconds}s/shape"
ci_summary ""
while IFS= read -r line; do ci_summary "$line"; done <"$tmp/table.md"
ci_summary ""
ci_summary "Smoke pipeline sanity: **$smoke**"
[ "$smoke" = "FAILED" ] && exit 1
exit 0
