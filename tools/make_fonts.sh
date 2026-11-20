#!/bin/sh
# Generates romfs/fonts/inter-<px>.bcfnt from Inter TTFs (https://rsms.me/inter,
# SIL Open Font License) with devkitPro's mkbcfnt.
#
# Usage: tools/make_fonts.sh <dir containing Inter-Medium.ttf and Inter-SemiBold.ttf>
# Run inside the devkitpro/devkitarm image if host tools can't run, e.g.
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD":/src -v <ttfdir>:/ttf -w /src \
#     devkitpro/devkitarm sh tools/make_fonts.sh /ttf
#
# Sizes must match SIZES[] in source/ui_font.c.
set -eu
TTF_DIR=${1:?path to Inter TTFs}
OUT=romfs/fonts
MKBCFNT=${MKBCFNT:-mkbcfnt}
mkdir -p "$OUT"

# Latin + Latin-1 + Latin Extended-A + common typographic punctuation:
# enough for European track/artist names. Anything else (CJK, 3DS button
# glyphs) falls back to the system font at draw time.
WL=$(mktemp)
{
	seq 32 126
	seq 160 383
	for cp in 8211 8212 8216 8217 8218 8220 8221 8222 8226 8230 8364 8482; do echo "$cp"; done
} | tr '\n' ' ' > "$WL"

# "px" is the line height the UI layout expects (source/ui.c sizes text by
# line height, like the system font). mkbcfnt takes points at 96 dpi and Inter's
# line gap is ~1.21 em, so line height ~= 1.613 x pt. Values verified so the
# generated lineFeed equals px exactly.
for spec in 11:6.82 12:7.44 13:8.06 14:8.68 15:9.30 18:11.16; do
	px=${spec%%:*}; pt=${spec#*:}
	# Medium reads better than Regular at small sizes on the 2DS's LCD;
	# SemiBold for the large track title.
	face=Inter-Medium.ttf
	[ "$px" -ge 18 ] && face=Inter-SemiBold.ttf
	"$MKBCFNT" -s "$pt" -w "$WL" -o "$OUT/inter-$px.bcfnt" "$TTF_DIR/$face"
done
rm -f "$WL"
cp "$TTF_DIR/LICENSE.txt" "$OUT/Inter-LICENSE.txt" 2>/dev/null || true
ls -la "$OUT"
