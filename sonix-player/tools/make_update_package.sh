#!/bin/sh
#
# Packs the player for over-the-air update: one file the device can verify and
# install without touching the vendor's firmware.
#
#   tools/make_update_package.sh <sonix_player> <version> <board-id> <out-dir>
#
# The notes the update card shows are the message of the annotated tag the
# version names: a release is one commit -- bump "version", write the tag message
# as the changelog, tag. NOTES_FILE overrides that for a test build that has no
# tag (a file whose whole content is the notes).
#
# The result, per device, is
#
#   <out-dir>/sonix-player-update-<board-id>.tar            the package
#   <out-dir>/sonix-player-update-<board-id>.tar.sha256     its digest
#   <out-dir>/<board-id>.json                               a fragment for index.json
#
# Inside the archive is the stripped binary and manifest.json, which names every
# file with its size, mode and sha256: the player refuses a package unless the
# manifest, the files, its own board and its own version all agree. The board id
# is board.h's update stem ("v3_analog"), not the human name: a second device
# differs in the panel, the legend and the update file name, so a package for one
# is a broken device on the other and the check has to be on the stable name.
#
# What is deliberately NOT in here: the launcher (sonix_launch) and the kernel,
# the rootfs or the vendor's bootloader. The launcher is the path back from a
# bad player, so it is a part of the image and only an image update may change
# it; the rest we have no right to redistribute (docs/BUILD.md, "Distributing an
# image").
#
# The package is a plain tar, not a tar.gz, and that is deliberate: the player
# has about a megabyte of free RAM while it runs (LVGL holds the rest), and
# inflating needs a 32 KB dictionary plus a streaming state machine inside the one
# program that must not run out of memory. 6.7 MB over Wi-Fi instead of 2.9 is the
# cheaper side of that trade. If compression ever comes back, it comes back with a
# streaming inflate tested on its own first.
#
# The package is reproducible: the same binary and version produce the same
# manifest, and the tar is written with fixed times and owners, so two builds of
# one commit are byte for byte the same.
set -eu

BINARY=${1:-}
VERSION=${2:-}
BOARD=${3:-}
OUT=${4:-}

if [ -z "$BINARY" ] || [ -z "$VERSION" ] || [ -z "$BOARD" ] || [ -z "$OUT" ]; then
	echo "usage: $0 <sonix_player> <version> <board-id> <out-dir>" >&2
	exit 2
fi
[ -f "$BINARY" ] || { echo "no binary at $BINARY" >&2; exit 1; }

# The notes: the annotated tag's message, or NOTES_FILE when the caller has one
# (a CI step building a version that is not tagged yet). Everything is escaped for
# JSON once, here, because the manifest and the index must say the same thing.
notes_from_tag() {
	if [ -n "${NOTES_FILE:-}" ] && [ -f "${NOTES_FILE}" ]; then
		cat "${NOTES_FILE}"
		return
	fi
	tag=${VERSION##*/}
	case "$tag" in
	'#'* | *-dirty | unknown | '') return 0 ;; # no tag names this build
	esac
	git -C "$(dirname "$0")/../../.." tag -l --format='%(contents)' "$tag" 2>/dev/null
}

json_escape() {
	# One JSON string body: backslashes first, then quotes and the whitespace a
	# tag message may carry. Blank lines at either end are dropped: a tag message
	# usually has one, the card does not want it.
	sed -e '/./,$!d' | sed -e ':a' -e '/^\n*$/{$d;N;ba' -e '}' |
		awk 'BEGIN { ORS = ""; first = 1 }
			{
				gsub(/\\/, "\\\\")
				gsub(/"/, "\\\"")
				gsub(/\t/, "\\t")
				gsub(/\r/, "")
				if (!first) printf "\\n"
				printf "%s", $0
				first = 0
			}'
}

NAME=sonix_player
PACKAGE=sonix-player-update-$BOARD.tar
SIZE=$(wc -c <"$BINARY" | tr -d ' ')
SHA=$(sha256sum "$BINARY" | cut -d' ' -f1)
mkdir -p "$OUT/stage"

# Fixed mode and name: the installer takes the manifest's word for both, and a
# binary that is not 0755 does not start.
cp "$BINARY" "$OUT/stage/$NAME"
chmod 755 "$OUT/stage/$NAME"
touch -d @0 "$OUT/stage/$NAME" 2>/dev/null || touch -t 197001010000 "$OUT/stage/$NAME"

NOTES_JSON=$(notes_from_tag | json_escape)
# The rootfs this package needs, when it needs one newer than what the device may
# be running (a launcher change, say). Empty means "any image".
REQUIRES_IMAGE="${REQUIRES_IMAGE:-}"

cat >"$OUT/stage/manifest.json" <<EOF
{
  "kind": "player",
  "format": 1,
  "board": "$BOARD",
  "version": "$VERSION",
  "requires_image": "$REQUIRES_IMAGE",
  "notes": "$NOTES_JSON",
  "files": [
    {
      "name": "$NAME",
      "size": $SIZE,
      "mode": "0755",
      "sha256": "$SHA"
    }
  ]
}
EOF

tar -C "$OUT/stage" --owner=0 --group=0 --numeric-owner --mtime=@0 \
	-cf "$OUT/$PACKAGE" manifest.json "$NAME"
(cd "$OUT" && sha256sum "$PACKAGE" >"$PACKAGE.sha256")

ARCHIVE_SHA=$(cut -d' ' -f1 "$OUT/$PACKAGE.sha256")
ARCHIVE_SIZE=$(wc -c <"$OUT/$PACKAGE" | tr -d ' ')

# One fragment per device, so a release can carry several and the index is a
# concatenation rather than a table maintained by hand.
cat >"$OUT/$BOARD.json" <<EOF
{
  "board": "$BOARD",
  "version": "$VERSION",
  "requires_image": "$REQUIRES_IMAGE",
  "notes": "$NOTES_JSON",
  "package": "$PACKAGE",
  "size": $ARCHIVE_SIZE,
  "sha256": "$ARCHIVE_SHA"
}
EOF

rm -rf "$OUT/stage"

echo "package: $OUT/$PACKAGE"
echo "  version:  $VERSION"
echo "  board:    $BOARD"
echo "  binary:   $SIZE bytes, sha256 $SHA"
echo "  archive:  $ARCHIVE_SHA"
