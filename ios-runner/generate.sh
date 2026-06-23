#!/usr/bin/env bash
# Copyright 2026, the gaby-vm authors
# SPDX-License-Identifier: BSD-3-Clause
#
# Generate the iOS runner's two Xcode projects:
#   1) the gaby_vm library project, via CMake's Xcode generator (the source list
#      stays single-sourced in CMakeLists.txt — no second copy to drift), and
#   2) the host app + XCTest project, via XcodeGen from ios-runner/project.yml,
#      which references the library project and depends on its `gaby_vm` target
#      so Xcode builds the library for the active destination.
#
# Between the two, we strip the legacy `PBXBuildStyle` objects CMake emits:
# XcodeGen's project parser rejects them, but Xcode itself does not need them.
# The strip is a structural plist edit (not a line/regex hack), so it survives
# CMake reformatting and version changes.
#
# Run this once after cloning, and again whenever the library source list or
# ios-runner/project.yml changes. Then open ios-runner/GabyRunner.xcodeproj.
# Both generated .xcodeproj bundles are git-ignored.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ios_dir="$repo_root/ios-runner"
lib_build="$repo_root/build/ios-xcode"

echo "[1/3] gaby_vm library Xcode project (cmake -G Xcode) -> $lib_build"
cmake -B "$lib_build" -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
  -DGABY_VM_BUILD_TESTS=OFF \
  -DGABY_VM_BUILD_DEMOS=OFF \
  -DGABY_VM_BUILD_BENCHMARKS=OFF \
  -DGABY_VM_BUILD_IOS_RUNNER=ON \
  "$repo_root" >/dev/null

echo "[2/3] strip legacy PBXBuildStyle (so XcodeGen can parse the project)"
pbxproj="$lib_build/gaby_vm.xcodeproj/project.pbxproj"
[ -f "$pbxproj" ] || { echo "error: $pbxproj not found" >&2; exit 1; }
# CMake writes the project as a legacy ASCII plist; convert to XML so plistlib
# can load it, then drop every PBXBuildStyle object and any dangling
# PBXProject.buildStyles reference to it.
plutil -convert xml1 "$pbxproj"
python3 - "$pbxproj" <<'PY'
import plistlib, sys
path = sys.argv[1]
with open(path, "rb") as f:
    proj = plistlib.load(f)
objects = proj.get("objects", {})
# Object types XcodeGen's parser rejects but Xcode ignores. Extend if a future
# CMake/Xcode pairing surfaces another one.
UNSUPPORTED_ISA = {"PBXBuildStyle"}
removed = [k for k, v in objects.items()
           if isinstance(v, dict) and v.get("isa") in UNSUPPORTED_ISA]
for k in removed:
    del objects[k]
for v in objects.values():
    if isinstance(v, dict):
        v.pop("buildStyles", None)  # only PBXProject carries this, references PBXBuildStyle
with open(path, "wb") as f:
    plistlib.dump(proj, f)
print(f"  removed {len(removed)} PBXBuildStyle object(s)")
PY
if grep -q "PBXBuildStyle" "$pbxproj"; then
  echo "error: PBXBuildStyle still present after strip" >&2
  exit 1
fi

# Local, untracked signing config (git-ignored). project.yml references it via
# configFiles, so XcodeGen needs it to exist. Write a placeholder once; a
# developer fills in DEVELOPMENT_TEAM for on-device runs (the Simulator needs
# none).
signing_xcconfig="$ios_dir/Signing.xcconfig"
if [ ! -f "$signing_xcconfig" ]; then
  cat > "$signing_xcconfig" <<'XCCONFIG'
// Local signing config for the iOS runner — git-ignored, never committed.
// To run on a physical device, set your Apple Development team here:
//   DEVELOPMENT_TEAM = ABCDE12345
// Leave it empty for the Simulator (no signing needed).
DEVELOPMENT_TEAM =
XCCONFIG
fi

echo "[3/3] host app + XCTest project (xcodegen)"
( cd "$ios_dir" && xcodegen generate )

echo "Done. Open: ios-runner/GabyRunner.xcodeproj"
