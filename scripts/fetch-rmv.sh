#!/usr/bin/env bash
# Fetches AMD's Radeon Memory Visualizer at a pinned release, plus the
# dependencies its parser and backend need. Nothing in AMD's tree is modified:
# this project's CMakeLists.txt only pulls in the subdirectories it uses.
#
#   scripts/fetch-rmv.sh [target directory]   default: ./radeon_memory_visualizer
#
# Afterwards:
#   cmake -S . -B build -G Ninja && cmake --build build
set -euo pipefail

RMV_REPO="https://github.com/GPUOpen-Tools/radeon_memory_visualizer.git"
# Tested release. The parser/backend API is not stable across releases, so
# bump this deliberately and re-test rather than tracking master.
RMV_TAG="v1.15"

root="$(cd "$(dirname "$0")/.." && pwd)"
target="${1:-$root/radeon_memory_visualizer}"

for tool in git python3; do
    command -v "$tool" >/dev/null || { echo "error: '$tool' is required" >&2; exit 1; }
done

if [ -d "$target/.git" ]; then
    have="$(git -C "$target" describe --tags --exact-match 2>/dev/null || echo unknown)"
    if [ "$have" != "$RMV_TAG" ]; then
        echo "error: $target exists but is at '$have', expected $RMV_TAG." >&2
        echo "       Remove it or check out $RMV_TAG yourself." >&2
        exit 1
    fi
    echo "Radeon Memory Visualizer $RMV_TAG already present in $target"
else
    git clone --depth 1 --branch "$RMV_TAG" "$RMV_REPO" "$target"
fi

# AMD's own script clones rdf, system_info_utils, nlohmann/json and a few
# others at the versions AMD pins in build/dependency_map.py. It also fetches
# the Qt helper libraries, which are not built here but are harmless.
(cd "$target/build" && python3 fetch_dependencies.py)

echo
echo "Done. Now build with:"
echo "  cmake -S \"$root\" -B \"$root/build\" -G Ninja"
echo "  cmake --build \"$root/build\""
