#!/bin/sh
#
# Takes one release payload apart into the two halves the site serves.
#
#   make_site_payload.sh <payload.tar.gz> <site-dir> <name>
#
# The release asset (`firmware-payload-<board>.tar.gz`, attached by
# .github/workflows/player-package.yml) stays the one complete archive; this
# script only turns it into what a browser can fetch from the site's own origin,
# because a release asset cannot be fetched across origins (no
# `Access-Control-Allow-Origin` on the redirect to release-assets.githubusercontent.com).
#
#   payload/<name>.player.tar.gz          payload.json, sonix_player, sonix_launch
#                                         (~3 MB, differs per version)
#   payload/blobs/assets-<sha256:12>.tar.gz
#                                         assets/BLAZE + assets/V3ANALOG, named by
#                                         content, so every version whose assets
#                                         did not change points at the same blob
#                                         (~22 MB once, cached by the browser)
#   payload/<name>.assets                 which blob this version uses, with its
#                                         sha256 (one line: path digest)
#
# Both halves are written with sorted names, zeroed owners and a fixed mtime, so
# the same tree always produces the same blob name -- that is what makes the
# deduplication work between releases.
set -eu

PAYLOAD=${1:-}
SITE=${2:-}
NAME=${3:-}

if [ -z "$PAYLOAD" ] || [ -z "$SITE" ] || [ -z "$NAME" ]; then
	echo "usage: $0 <payload.tar.gz> <site-dir> <name>" >&2
	exit 2
fi
[ -f "$PAYLOAD" ] || { echo "make_site_payload: no $PAYLOAD" >&2; exit 1; }

PAYLOAD_DIR="$SITE/payload"
mkdir -p "$PAYLOAD_DIR/blobs"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
tar -xzf "$PAYLOAD" -C "$TMP"

for f in payload.json sonix_player sonix_launch; do
	[ -f "$TMP/$f" ] || { echo "make_site_payload: $PAYLOAD has no $f" >&2; exit 1; }
done
[ -d "$TMP/assets" ] || { echo "make_site_payload: $PAYLOAD has no assets/" >&2; exit 1; }

tar_it() {
	# $1: output; the rest: what to pack, relative to $TMP
	out=$1
	shift
	tar -C "$TMP" -cf - --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 "$@" |
		gzip -9 -n >"$out"
}

tar_it "$TMP/assets.tar.gz" assets
assets_sha=$(sha256sum "$TMP/assets.tar.gz" | cut -d' ' -f1)
blob="assets-$(printf '%s' "$assets_sha" | cut -c1-12).tar.gz"
if [ -f "$PAYLOAD_DIR/blobs/$blob" ]; then
	# Same content, already here: the whole point of naming blobs by their digest.
	:
else
	mv "$TMP/assets.tar.gz" "$PAYLOAD_DIR/blobs/$blob"
fi
printf '%s %s\n' "blobs/$blob" "$assets_sha" >"$PAYLOAD_DIR/$NAME.assets"

tar_it "$PAYLOAD_DIR/$NAME.player.tar.gz" payload.json sonix_player sonix_launch

printf 'make_site_payload: %s -> player %s bytes, assets %s (%s)\n' \
	"$NAME" "$(wc -c <"$PAYLOAD_DIR/$NAME.player.tar.gz" | tr -d ' ')" \
	"$blob" "$(printf '%s' "$assets_sha" | cut -c1-12)"
