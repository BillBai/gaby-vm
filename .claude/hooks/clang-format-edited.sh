#!/usr/bin/env bash
# PostToolUse hook: format the file just edited by Claude with clang-format.
#
# Triggers on Edit / Write / MultiEdit. Reads the tool payload from stdin
# (JSON), extracts tool_input.file_path, and runs `clang-format -i` only
# when that path has a C/C++ extension. Always exits 0 — missing tools or
# non-matching paths must not block edits.

set -u

# jq is required to extract file_path from the stdin JSON. If jq is missing,
# silently skip — the project is not yet jq-required.
if ! command -v jq >/dev/null 2>&1; then
  exit 0
fi

file=$(jq -r '.tool_input.file_path // empty' 2>/dev/null)

case "$file" in
  '')
    exit 0
    ;;
  *.c | *.cc | *.cpp | *.cxx | *.h | *.hpp | *.hxx)
    if command -v clang-format >/dev/null 2>&1; then
      clang-format -i -- "$file" 2>/dev/null || true
    fi
    ;;
esac

exit 0
