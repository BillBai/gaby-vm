#!/usr/bin/env bash
# Copyright 2026, the gaby-vm authors
# SPDX-License-Identifier: BSD-3-Clause
#
# ci/util.sh — shared helpers sourced by the gaby-vm CI scripts.
#
# This file is sourced, not executed: it defines helper functions and the
# CI_REPO_ROOT variable. Every ci/ script does
#   source "$(dirname "${BASH_SOURCE[0]}")/util.sh"
# Targets bash 3.2 (the macOS system bash): no associative arrays, no ${x,,}.

# Absolute path to the repository root (this file lives in <root>/ci/).
CI_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ci_build_dir <preset> -> absolute build directory for that CMake preset.
# dev-debug -> <root>/build/debug, dev-release -> <root>/build/release.
ci_build_dir() {
  printf '%s/build/%s\n' "$CI_REPO_ROOT" "${1#dev-}"
}

# ci_log <msg...> -> timestamped progress line on stderr (not report content).
ci_log() {
  printf '[ci] %s\n' "$*" >&2
}

# ci_file_bytes <path> -> size in bytes (0 if missing). macOS + Linux stat.
ci_file_bytes() {
  if [ -f "$1" ]; then
    stat -f%z "$1" 2>/dev/null || stat -c%s "$1" 2>/dev/null || echo 0
  else
    echo 0
  fi
}

# ci_human_kb <bytes> -> "123.4 KB"
ci_human_kb() {
  awk -v b="${1:-0}" 'BEGIN { printf "%.1f KB", b / 1024 }'
}

# ci_find <dir> <name> -> first matching regular file under dir (may be empty).
ci_find() {
  [ -d "$1" ] || { printf ''; return 0; }
  find "$1" -name "$2" -type f 2>/dev/null | head -1
}

# ci_summary <markdown...> -> append to the GitHub job summary when running
# under Actions; otherwise echo to stdout so local runs still show the report.
# Multi-line arguments are preserved.
ci_summary() {
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    printf '%s\n' "$*" >>"$GITHUB_STEP_SUMMARY"
  else
    printf '%s\n' "$*"
  fi
}

# ci_sticky_comment <pr-number> <body-file> -> create or update THE CI comment
# on a pull request. Uses `gh pr comment --edit-last` (the Actions bot is the
# only author that posts the CI comment, so "edit last" reliably targets it);
# falls back to a fresh comment on the first run. No-op (returns 0) when gh is
# unavailable or no PR number is given, so local runs are unaffected.
ci_sticky_comment() {
  local pr="${1:-}" body_file="${2:-}"
  command -v gh >/dev/null 2>&1 || { ci_log "gh not available; skipping PR comment"; return 0; }
  [ -n "$pr" ] || { ci_log "no PR number; skipping PR comment"; return 0; }
  [ -f "$body_file" ] || { ci_log "comment body file missing; skipping"; return 0; }
  if gh pr comment "$pr" --body-file "$body_file" --edit-last >/dev/null 2>&1; then
    ci_log "updated sticky CI comment on PR #$pr"
  elif gh pr comment "$pr" --body-file "$body_file" >/dev/null 2>&1; then
    ci_log "created CI comment on PR #$pr"
  else
    ci_log "failed to post PR comment (non-fatal)"
  fi
}
