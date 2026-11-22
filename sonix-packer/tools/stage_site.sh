#!/bin/sh
#
# Stages `_site/` the way `.github/workflows/pages.yml` does, so the page can be
# tried locally before it is published.
#
#   sonix-packer/tools/stage_site.sh          # stage
#   cd _site && python3 -m http.server 8000   # then open http://localhost:8000/
#
# What it needs, and does not build: `sonix-packer/sonix_player` and
# `sonix_launch` (from `sonix-player/docker/build.sh`, or copied by the packer),
# and `sonix-packer/build-wasm/out/` (from `sonix-packer/tools/wasm/build.sh`).
# Both say so by name if they are missing.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" >/dev/null 2>&1 && pwd)"
PACKER_DIR="$(cd "$SCRIPT_DIR/.." >/dev/null 2>&1 && pwd)"
REPO="$(cd "$PACKER_DIR/.." >/dev/null 2>&1 && pwd)"
SITE="$REPO/_site"

for f in sonix_player sonix_launch; do
	[ -f "$PACKER_DIR/$f" ] || { echo "stage_site: no sonix-packer/$f -- build the player first" >&2; exit 1; }
done
[ -f "$PACKER_DIR/build-wasm/out/mkfs_ubifs.wasm" ] ||
	{ echo "stage_site: no build-wasm/out/mkfs_ubifs.wasm -- run sonix-packer/tools/wasm/build.sh" >&2; exit 1; }

version=$(make -s -C "$REPO/sonix-player" print-version BOARD=tempotec_v3)
stamp=$(date -r "$PACKER_DIR/sonix_player" +%d%m%Y%H%M)

rm -rf "$SITE"
mkdir -p "$SITE/wasm" "$SITE/payload"
cp -r "$REPO/web/." "$SITE/"
cp "$PACKER_DIR/build-wasm/out/mkfs_ubifs.js" "$PACKER_DIR/build-wasm/out/mkfs_ubifs.wasm" "$SITE/wasm/"
"$SCRIPT_DIR/make_payload.sh" "$SITE/payload/payload.tar.gz" "$version" "$stamp"

echo "stage_site: $SITE ($(du -sh "$SITE" | cut -f1)), version $version"
echo "           cd $SITE && python3 -m http.server 8000"
