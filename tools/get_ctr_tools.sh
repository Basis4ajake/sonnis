#!/bin/sh
# Downloads makerom and bannertool (needed only for `make cia`) into
# tools/bin/, pinned to exact releases and verified by SHA-256 so a changed
# or tampered download fails instead of ending up in a release.
#
# Linux x86_64 builds; run inside the devkitpro/devkitarm image if the host
# can't run them:
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD":/src -w /src \
#     devkitpro/devkitarm sh tools/get_ctr_tools.sh
set -eu

MAKEROM_URL=https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.19.0/makerom-v0.19.0-ubuntu_x86_64.zip
MAKEROM_SHA=287b809dec064e0ad597e3d272c49ecb7eed41693d5ee6fef9d8a8aa24c2497e
BANNERTOOL_URL=https://github.com/carstene1ns/3ds-bannertool/releases/download/1.2.3/bannertool-1.2.3-linux.tar.gz
BANNERTOOL_SHA=748519d200519db18e9fd00c332ac32f5411e41230258291e89953a76c1f7155

BIN=$(cd "$(dirname "$0")" && pwd)/bin
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$BIN"

fetch() { # url sha256 file
	curl -fsSL -o "$TMP/$3" "$1"
	echo "$2  $TMP/$3" | sha256sum -c - >/dev/null || {
		echo "checksum mismatch for $1" >&2
		exit 1
	}
}

fetch "$MAKEROM_URL" "$MAKEROM_SHA" makerom.zip
unzip -qo "$TMP/makerom.zip" makerom -d "$BIN"

fetch "$BANNERTOOL_URL" "$BANNERTOOL_SHA" bannertool.tar.gz
tar xzf "$TMP/bannertool.tar.gz" -C "$TMP"
cp "$TMP"/bannertool-*/bannertool "$BIN/"

chmod +x "$BIN/makerom" "$BIN/bannertool"
echo "installed makerom, bannertool in tools/bin"
