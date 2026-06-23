#!/usr/bin/env bash
# Copyright 2026, the gaby-vm authors
# SPDX-License-Identifier: BSD-3-Clause
#
# ci/ios-bench.sh — run the iOS business benchmark on an arm64 iOS Simulator and
# report its cache/decoder numbers. REPORT-ONLY: it never fails the run (timing
# is too noisy to gate, and the iOS runner has no native-baseline track). On any
# non-arm64 host it SKIPS loudly and exits 0.
#
# Built in Release: the numbers are only representative optimised, and Xcode's
# default Debug runs roughly an order of magnitude slower per instruction. (The
# correctness gate, ci/ios-test.sh, stays on the default Debug — timing is not
# its concern.)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/util.sh"

arch="$(uname -m)"
if [ "$arch" != "arm64" ]; then
  ci_summary "### ⏭️ iOS benchmark — SKIPPED (not run)"
  ci_summary ""
  ci_summary "Runner architecture is \`$arch\`, not \`arm64\`, so no arm64 iOS"
  ci_summary "Simulator is available. The iOS benchmark **did not run**."
  ci_log "iOS bench skipped: runner arch '$arch' is not arm64"
  exit 0
fi

cd "$CI_REPO_ROOT"

if ! command -v xcodegen >/dev/null 2>&1; then
  ci_log "installing xcodegen"
  brew install xcodegen >/dev/null
fi

ci_log "generating iOS runner projects"
./ios-runner/generate.sh >/dev/null

udid="$(xcrun simctl list devices available \
  | grep -E '^[[:space:]]+iPhone' \
  | grep -oE '[0-9A-Fa-f]{8}-([0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}' \
  | head -1 || true)"
if [ -z "$udid" ]; then
  ci_summary "### ⏭️ iOS benchmark — SKIPPED (no iPhone simulator available)"
  ci_log "iOS bench skipped: no iPhone simulator available"
  exit 0
fi
ci_log "using iOS Simulator $udid"

log_file="$(mktemp)"
set +e
xcodebuild test \
  -project ios-runner/GabyRunner.xcodeproj \
  -scheme GabyRunner \
  -configuration Release \
  -destination "platform=iOS Simulator,id=$udid" \
  -only-testing:GabyRunnerTests/BenchTests 2>&1 | tee "$log_file"
status=${PIPESTATUS[0]}
set -e

# The harness prints a block of `key: value` lines per kernel per mode. Keep the
# metric-bearing ones for the report.
report="$(grep -E '(^|[[:space:]])(workload|mode|iterations_per_second|throughput_insn_per_sec|ns_per_instruction):' "$log_file" || true)"
rm -f "$log_file"

ci_summary "### 📊 iOS benchmark — cache vs decoder (report-only)"
ci_summary ""
if [ -n "$report" ]; then
  ci_summary "\`\`\`"
  ci_summary "$report"
  ci_summary "\`\`\`"
else
  ci_summary "_(no benchmark numbers captured; xcodebuild status=$status)_"
fi

# Report-only: a benchmark result never fails the run.
exit 0
