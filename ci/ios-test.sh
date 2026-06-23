#!/usr/bin/env bash
# Copyright 2026, the gaby-vm authors
# SPDX-License-Identifier: BSD-3-Clause
#
# ci/ios-test.sh — build the iOS runner and run its XCTest suites on an arm64
# iOS Simulator. This is a DETERMINISTIC gate: a test failure fails (non-zero).
#
# It runs only where an arm64 iOS Simulator is available (an arm64 macOS host).
# On any other host — e.g. an x86 runner — it SKIPS loudly and explicitly,
# reporting "did not run / skipped" to the job summary and exiting 0. A skip is
# never a silent pass.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/util.sh"

arch="$(uname -m)"
if [ "$arch" != "arm64" ]; then
  ci_summary "### ⏭️ iOS Simulator tests — SKIPPED (not run)"
  ci_summary ""
  ci_summary "Runner architecture is \`$arch\`, not \`arm64\`, so no arm64 iOS"
  ci_summary "Simulator is available. The iOS tests **did not run** — this is a"
  ci_summary "skip, not a pass."
  ci_log "iOS tests skipped: runner arch '$arch' is not arm64"
  exit 0
fi

cd "$CI_REPO_ROOT"

# The runner projects are generated, not committed. generate.sh needs XcodeGen
# (authoring-only); install it on demand so the script is self-contained.
if ! command -v xcodegen >/dev/null 2>&1; then
  ci_log "installing xcodegen"
  brew install xcodegen >/dev/null
fi

ci_log "generating iOS runner projects"
./ios-runner/generate.sh >/dev/null

# Pick the first available iPhone simulator (any runtime). Its UDID maps to a
# concrete runtime/arch, which xcodebuild boots automatically.
udid="$(xcrun simctl list devices available \
  | grep -E '^[[:space:]]+iPhone' \
  | grep -oE '[0-9A-Fa-f]{8}-([0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}' \
  | head -1 || true)"

if [ -z "$udid" ]; then
  ci_summary "### ⏭️ iOS Simulator tests — SKIPPED (not run)"
  ci_summary ""
  ci_summary "No iPhone Simulator is available on this arm64 runner, so the iOS"
  ci_summary "tests **did not run** — this is a skip, not a pass."
  ci_log "iOS tests skipped: no iPhone simulator available"
  exit 0
fi
ci_log "using iOS Simulator $udid"

log_file="$(mktemp)"
set +e
xcodebuild test \
  -project ios-runner/GabyRunner.xcodeproj \
  -scheme GabyRunner \
  -destination "platform=iOS Simulator,id=$udid" \
  -skip-testing:GabyRunnerTests/BenchTests 2>&1 | tee "$log_file"
status=${PIPESTATUS[0]}
set -e

# "Executed N tests, with M failures (...)" is XCTest's tally line.
tally="$(grep -E 'Executed [0-9]+ test' "$log_file" | tail -1 | sed 's/^[[:space:]]*//')"
rm -f "$log_file"

if [ "$status" -eq 0 ]; then
  ci_summary "### ✅ iOS Simulator tests"
else
  ci_summary "### ❌ iOS Simulator tests"
fi
ci_summary ""
ci_summary "\`\`\`"
ci_summary "${tally:-xcodebuild produced no tally line}"
ci_summary "\`\`\`"

exit "$status"
