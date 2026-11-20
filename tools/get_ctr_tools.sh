#!/bin/sh
# Builds makerom and bannertool (needed only for `make cia`) from source into
# tools/bin/, at exact commits that were security-reviewed on 2026-11-20:
#
#   makerom    3DSGuy/Project_CTR        makerom-v0.19.0  e8f5f529c54f...
#   bannertool carstene1ns/3ds-bannertool 1.2.3           da573b13418a...
#
# Review covered: both tools' own source (no network/exec/env access, writes
# only the -o output), all build scripts (compile + link only; bannertool's
# update-libs target, which downloads, is never invoked), and the bundled
# libraries (mbedTLS = upstream 2.16.5 + RSA_NO_CRT; libyaml = 0.1.4 with
# rename-only edits; utfcpp/stb/dr_wav byte-identical to upstream commits).
#
# Building from source instead of downloading release binaries means nothing
# opaque is trusted: what runs is the reviewed code. Fetching by commit hash
# (and checking it) means a moved tag or rewritten branch can't swap it.
#
# Needs git, make, gcc/g++, cmake >= 3.28 and ninja: all present in the
# devkitpro/devkitarm image the project already builds with:
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD":/src -w /src \
#     devkitpro/devkitarm sh tools/get_ctr_tools.sh
set -eu

MAKEROM_REPO=https://github.com/3DSGuy/Project_CTR.git
MAKEROM_COMMIT=e8f5f529c54ff9b22a2491a480ffa69206bf7b19
BANNERTOOL_REPO=https://github.com/carstene1ns/3ds-bannertool.git
BANNERTOOL_COMMIT=da573b13418acd0ac1803b43b30e8a155e5f83e2

BIN=$(cd "$(dirname "$0")" && pwd)/bin
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export HOME="$WORK" # git needs one; the container user may have none
mkdir -p "$BIN"

# fetch <repo> <commit> <dir>: shallow-fetch exactly that commit, verify it.
fetch() {
	git init -q "$3"
	git -C "$3" fetch -q --depth 1 "$1" "$2"
	git -C "$3" -c advice.detachedHead=false checkout -q FETCH_HEAD
	got=$(git -C "$3" rev-parse HEAD)
	if [ "$got" != "$2" ]; then
		echo "commit mismatch for $1: wanted $2, got $got" >&2
		exit 1
	fi
}

JOBS=$(nproc 2>/dev/null || echo 2)

echo "building makerom ${MAKEROM_COMMIT%"${MAKEROM_COMMIT#????????????}"}..."
fetch "$MAKEROM_REPO" "$MAKEROM_COMMIT" "$WORK/ctr"
make -s -C "$WORK/ctr/makerom" -j"$JOBS" deps >/dev/null
make -s -C "$WORK/ctr/makerom" -j"$JOBS" program >/dev/null
cp "$WORK/ctr/makerom/bin/makerom" "$BIN/"

echo "building bannertool ${BANNERTOOL_COMMIT%"${BANNERTOOL_COMMIT#????????????}"}..."
fetch "$BANNERTOOL_REPO" "$BANNERTOOL_COMMIT" "$WORK/bt"
cmake -S "$WORK/bt" -B "$WORK/bt/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/bt/build" >/dev/null
cp "$WORK/bt/build/bannertool" "$BIN/"

echo "installed makerom, bannertool in tools/bin (built from reviewed commits)"
