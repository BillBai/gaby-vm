#!/usr/bin/env bash
# Copyright 2026, the gaby-vm authors
# SPDX-License-Identifier: BSD-3-Clause
#
# ci/build.sh <preset> [extra cmake -D args...]
#
# Configure + build a CMake preset. Extra args are passed to the configure
# step, e.g.:
#   ci/build.sh dev-debug
#   ci/build.sh dev-release -DGABY_VM_BUILD_BENCHMARKS=ON
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/util.sh"

preset="${1:?usage: build.sh <preset> [extra -D args]}"
shift || true

cd "$CI_REPO_ROOT"
ci_log "configure: cmake --preset $preset $*"
cmake --preset "$preset" "$@"
ci_log "build: cmake --build --preset $preset"
cmake --build --preset "$preset"
ci_log "build done -> $(ci_build_dir "$preset")"
