#!/usr/bin/env bash
# Stop hook: build and run tests at the end of every Claude turn.
#
# Skips silently if cmake/ctest aren't installed or the dev-debug build dir
# hasn't been created on this machine. Otherwise runs an incremental build
# and ctest; if either fails, prints the captured output and exits non-zero
# so Claude has to address the failure before the turn completes. On
# success the hook is silent — quiet on green, loud on red.

set -u

cd "${CLAUDE_PROJECT_DIR:-$(pwd)}"

# Skip on machines without a CMake toolchain (docs-only contributors etc).
if ! command -v cmake >/dev/null 2>&1 || ! command -v ctest >/dev/null 2>&1; then
  exit 0
fi

# Skip if configure hasn't been run yet — nothing to build against.
if [ ! -d build/debug ]; then
  exit 0
fi

# Incremental build. Ninja short-circuits when nothing changed.
if ! output=$(cmake --build --preset dev-debug 2>&1); then
  printf '[stop-hook] build failed:\n%s\n' "$output" >&2
  exit 1
fi

# --output-on-failure dumps the failing test's own stdout/stderr; we also
# capture-and-echo so Claude sees the full picture in one place.
if ! output=$(ctest --preset dev-debug --output-on-failure 2>&1); then
  printf '[stop-hook] tests failed:\n%s\n' "$output" >&2
  exit 2
fi

exit 0
