#!/usr/bin/env bash
# Fetches AMD's Radeon Memory Visualizer at a pinned release, plus the
# dependencies its parser and backend need. Nothing in AMD's tree is modified:
# this project's CMakeLists.txt only pulls in the subdirectories it uses.
#
#   scripts/fetch-rmv.sh [--latest | --tag TAG] [target directory]
#
#   default      the tested release pinned below
#   --latest     the newest vX.Y release tag of AMD's repository (untested)
#   --tag TAG    a specific release tag
#   target directory defaults to ./radeon_memory_visualizer
#
# Afterwards:
#   cmake -S . -B build -G Ninja && cmake --build build
set -euo pipefail

RMV_REPO="https://github.com/GPUOpen-Tools/radeon_memory_visualizer.git"
# Tested release. Pinned rather than "latest" on purpose: this project's
# CMakeLists.txt re-creates part of AMD's top-level build setup, and AMD
# changes its build files and dependency versions in almost every release.
# A pinned tag keeps a given commit of rmv-report building the same way.
# Bump it deliberately after building and comparing output.
RMV_TAG="v1.15"

root="$(cd "$(dirname "$0")/.." && pwd)"
latest=0
while [ $# -gt 0 ]; do
    case "$1" in
        --latest) latest=1; shift ;;
        --tag)    [ $# -ge 2 ] || { echo "error: --tag needs a value" >&2; exit 2; }
                  RMV_TAG="$2"; shift 2 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        -*)       echo "error: unknown option $1" >&2; exit 2 ;;
        *)        break ;;
    esac
done
target="${1:-$root/radeon_memory_visualizer}"

for tool in git python3; do
    command -v "$tool" >/dev/null || { echo "error: '$tool' is required" >&2; exit 1; }
done

if [ "$latest" = 1 ]; then
    RMV_TAG="$(git ls-remote --tags --refs "$RMV_REPO" 'v*' \
        | sed 's#.*refs/tags/##' | grep -E '^v[0-9]+\.[0-9]+$' | sort -V | tail -1)"
    [ -n "$RMV_TAG" ] || { echo "error: could not determine the latest release" >&2; exit 1; }
    echo "Latest release: $RMV_TAG (rmv-report is tested with the pinned default)"
fi

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
