#!/bin/sh
#
# Packs the files the firmware page needs that the vendor image does not carry.
#
#   make_payload.sh <out.tar.gz> <version> <build-stamp>
#
# The page builds the whole `.upt` in the browser, on the user's machine: it has
# the vendor's image, and everything else comes from here. What that "everything
# else" is:
#
#   sonix_player, sonix_launch     this port's binaries, from sonix-player/
#   assets/BLAZE, assets/V3ANALOG  the interface, the languages, the Bluetooth
#                                  userland -- the same trees v3_analog_packer.sh
#                                  installs over the stock rootfs
#   payload.json                   what the page needs to know about them
#
# This is the port's own material and nothing of the vendor's (docs/BUILD.md,
# "Distributing an image"): the stock firmware, the kernel and the bootloader are
# the user's own copy.
#
# `version` is what the player shows as "Operating system version" -- the packer's
# `<base>/<tag or commit>`, from `make print-version`; `build-stamp` is
# `date -r sonix_player +%d%m%Y%H%M`, the same value install_tree.sh writes.
#
# The archive is written with sorted names, zeroed owners and a fixed mtime, so a
# rebuild of one commit produces the same file.
set -eu

OUT=${1:-}
VERSION=${2:-}
STAMP=${3:-}

if [ -z "$OUT" ] || [ -z "$VERSION" ] || [ -z "$STAMP" ]; then
	echo "usage: $0 <out.tar.gz> <version> <build-stamp>" >&2
	exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" >/dev/null 2>&1 && pwd)"
PACKER_DIR="$(cd "$SCRIPT_DIR/.." >/dev/null 2>&1 && pwd)"

[ -f "$PACKER_DIR/sonix_player" ] || { echo "make_payload: no sonix_player (build it first)" >&2; exit 1; }
[ -f "$PACKER_DIR/sonix_launch" ] || { echo "make_payload: no sonix_launch" >&2; exit 1; }
[ -d "$PACKER_DIR/assets/BLAZE" ] || { echo "make_payload: no assets/BLAZE" >&2; exit 1; }
[ -d "$PACKER_DIR/assets/V3ANALOG" ] || { echo "make_payload: no assets/V3ANALOG" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/assets"

cp "$PACKER_DIR/sonix_player" "$PACKER_DIR/sonix_launch" "$WORK/"
cp -a "$PACKER_DIR/assets/BLAZE" "$PACKER_DIR/assets/V3ANALOG" "$WORK/assets/"

# The identity the image carries. The stamp is not decoration: the player offers an
# update only when it moves, and the update check compares it.
cat > "$WORK/payload.json" <<EOF
{
    "format": 1,
    "board": "v3_analog",
    "version": "$VERSION",
    "build_stamp": "$STAMP",
    "player": "sonix_player",
    "launcher": "sonix_launch",
    "assets": ["assets/BLAZE", "assets/V3ANALOG"]
}
EOF

mkdir -p "$(dirname "$OUT")"
tar -C "$WORK" -cf - --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 . |
	gzip -9 -n > "$OUT"

printf 'make_payload: %s (%s bytes), version %s, built %s\n' \
	"$OUT" "$(wc -c <"$OUT" | tr -d ' ')" "$VERSION" "$STAMP"
