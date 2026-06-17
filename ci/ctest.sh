#!/usr/bin/env bash
# Copyright 2026, the gaby-vm authors
# SPDX-License-Identifier: BSD-3-Clause
#
# ci/ctest.sh <preset>
#
# Run the FULL CTest suite for a built preset (every test, including the
# vixl_port guard rail). Emits a markdown result line to the job summary and
# propagates the ctest exit code: a test failure fails the gate.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/util.sh"

preset="${1:?usage: ctest.sh <preset>}"
build_dir="$(ci_build_dir "$preset")"

[ -d "$build_dir" ] || { ci_log "build dir missing: $build_dir (run ci/build.sh $preset first)"; exit 1; }

ci_log "ctest: $build_dir"
log_file="$(mktemp)"
set +e
ctest --test-dir "$build_dir" --output-on-failure 2>&1 | tee "$log_file"
status=${PIPESTATUS[0]}
set -e

summary_line="$(grep -E 'tests (passed|failed)' "$log_file" | tail -1)"
total_time="$(grep -E 'Total Test time' "$log_file" | tail -1 | sed 's/.*= *//')"
rm -f "$log_file"

if [ "$status" -eq 0 ]; then
  ci_summary "### ✅ Tests — \`$preset\`"
else
  ci_summary "### ❌ Tests — \`$preset\`"
fi
ci_summary ""
ci_summary "\`\`\`"
ci_summary "${summary_line:-ctest produced no summary line}"
[ -n "$total_time" ] && ci_summary "Total Test time = $total_time"
ci_summary "\`\`\`"

exit "$status"
