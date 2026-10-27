#!/usr/bin/env bash
# Build with the already-downloaded LG atlas SDK; no Docker image download.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")/.." && pwd)
sdk_root=${ATLAS_SDK_ROOT:-"$HOME/atlas-sdk-nuni-20261024"}
build_dir=${NUNI_BUILD_DIR:-"$HOME/nuni_atlas_build"}
if [ ! -f "$sdk_root/environment-setup-armv8a-atlas-linux" ]; then
  printf 'Set ATLAS_SDK_ROOT to the installed atlas SDK directory.\n' >&2
  exit 1
fi
set +u
source "$sdk_root/environment-setup-armv8a-atlas-linux"
set -u
cmake -S "$source_dir" -B "$build_dir" -DNUNI_REQUIRE_ALL=ON
cmake --build "$build_dir" --parallel "${NUNI_BUILD_JOBS:-4}"
printf '\nARM64 artifacts: %s/nunifusion and %s/nunisensors\n' "$build_dir" "$build_dir"
