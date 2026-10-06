#!/bin/sh
# fetch-stb.sh — fetch the stb single-file libraries nevermore needs.
#
# stb has no version tags, so we pin a specific commit hash (the same
# one ../coffer uses, so the two projects decode identically). The two
# headers land in third_party/stb/ where src/Makefile.am's
# -I$(top_srcdir)/third_party/stb picks them up:
#
#   stb_image.h        decode PNG/JPEG/GIF/... to RGBA (image_decode)
#   stb_image_write.h  encode RGBA to PNG (the transcode's re-encode)
#
# Both are public domain (Unlicense). The headers are NOT committed —
# configure.ac runs this script when they are missing, and .gitignore
# keeps them out of the tree.
#
# Usage: scripts/fetch-stb.sh [DEST_DIR]
#   DEST_DIR defaults to third_party/stb (relative to repo root)

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST="${1:-$SRC_DIR/third_party/stb}"

# Pinned commit — update when bumping stb (and keep coffer in step).
STB_COMMIT="2c980bb59875b0d32144a71867fbdebb2f77cd20"
BASE_URL="https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}"

mkdir -p "$DEST"

fetch() {
	header="$1"
	echo "Fetching $header ..."
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL "${BASE_URL}/${header}" -o "${DEST}/${header}"
	elif command -v wget >/dev/null 2>&1; then
		wget -q "${BASE_URL}/${header}" -O "${DEST}/${header}"
	else
		echo "ERROR: Need curl or wget to fetch stb headers" >&2
		exit 1
	fi
}

fetch stb_image.h
fetch stb_image_write.h

echo "Done. stb headers placed in ${DEST}"
echo "Pinned to commit ${STB_COMMIT}"
