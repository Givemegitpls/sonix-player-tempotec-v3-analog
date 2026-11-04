#!/bin/sh
#
# Regenerates src/addon/exports.list: the symbols the player publishes for
# addons, whether or not an addon happens to call them.
#
# Why a checked-in list and not a rule the build runs: the player's link needs
# the list, and the list is derived from the player's objects, so generating it
# during the build would be a cycle. This script is the deliberate step -- run it
# after `make target` when the interface modules change, look at the size, and
# commit the result. The addons' repository documents what may and may not be in
# it (sonix-player-addons, docs/ADDONS.md, "What an addon may call").
#
# The list has two halves:
#
#   the modules      every symbol of the modules an addon is documented to use,
#                    plus LVGL's public widgets. This is what makes an addon a
#                    card-only affair: one written against the documented headers
#                    finds what it calls already exported.
#   exports.addons   whatever else an addon asked for and the modules left out.
#                    It is a plain sorted list, checked in, appended verbatim; a
#                    name the player does not define is ignored by the linker, so
#                    the libc names that end up here (an addon's own imports) do
#                    nothing. Growing it is how an addon reaches a symbol the
#                    modules do not carry -- see the paragraph above the list in
#                    docs, and prefer the modules when the symbol belongs there.
#
# gui/nowplaying and gui/streaming are in the module list for the covers: a
# service page asks listart for the sleeve on a row and gets a cover_image_t
# back, and both live in those directories.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
player=$(cd "$here/.." && pwd)
build="$player/build_target"
out="$player/src/addon/exports.list"
extras="$player/src/addon/exports.addons"

NM=$(command -v mipsel-rockbox-linux-gnu-nm 2>/dev/null || true)
if [ -z "$NM" ]; then
	NM="$player/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu-nm"
fi
if [ ! -x "$NM" ]; then
	echo "$0: no mipsel nm found; set up the toolchain or PATH" >&2
	exit 1
fi
if [ ! -d "$build" ]; then
	echo "$0: $build does not exist -- run 'make target' first" >&2
	exit 1
fi
if [ ! -f "$extras" ]; then
	echo "$0: $extras is missing; it is checked in, and empty is a valid content" >&2
	exit 1
fi

player_objects="
	$build/gui/shell
	$build/gui/fonts
	$build/gui/nowplaying
	$build/gui/streaming
	$build/system/core
	$build/system/net
	$build/system/image
	$build/system/streaming
	$build/system/audio
	$build/system/library
	$build/system/playback
	$build/system/device
"

lvgl_objects="
	$build/lvgl/src/core
	$build/lvgl/src/widgets
	$build/lvgl/src/misc
	$build/lvgl/src/display
	$build/lvgl/src/indev
	$build/lvgl/src/draw
	$build/lvgl/src/layouts
	$build/lvgl/src/font
	$build/lvgl/src/stdlib
	$build/lvgl/src/tick
"

files=""
for dir in $player_objects $lvgl_objects; do
	if [ -d "$dir" ]; then
		files="$files $(find "$dir" -name '*.o' | tr '\n' ' ')"
	fi
done

tmp="$out.tmp"
{
	echo "/* The symbols the player publishes for addons: tools/addon_exports.sh writes this."
	echo "   The modules an addon is documented to use, LVGL's public widgets, and the"
	echo "   extras in exports.addons. Regenerated only when those modules change. */"
	echo "{"
	# shellcheck disable=SC2086
	$NM -g --defined-only $files 2>/dev/null |
		awk 'NF == 3 && $3 !~ /^_/ && $2 ~ /^[TDBRWV]$/ { print $3 }' |
		sed 's/@.*//' | sort -u | sed 's/^/  /; s/$/;/'
	sed 's/^/  /; s/$/;/' "$extras"
	echo "};"
} >"$tmp"

count=$(grep -c ';' "$tmp")
mv "$tmp" "$out"
echo "exports.list: $count names from $(echo $files | wc -w) objects and $(wc -l <"$extras") extras"
echo "rebuild the player ('make target') so the new list takes effect"
