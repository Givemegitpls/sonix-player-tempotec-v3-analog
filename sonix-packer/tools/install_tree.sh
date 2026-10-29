#!/bin/bash
#
# Runs inside the packer's container, as root, with the packer directory as the
# working directory. Installs the player, its launcher and this device's files
# into build/SYSTEM -- the tree that was just extracted from the stock image --
# and leaves it ready for mkfs.ubifs.
#
# Called by v3_analog_packer.sh, which explains the image as a whole.
set -euo pipefail

TREE=build/SYSTEM
BASE=assets/BLAZE	   # the V3 Blaze's interface assets: another player, this port's base
MODEL=assets/V3ANALOG  # and this device's own over them

[ -d "$TREE" ] || { echo "install_tree: $TREE is missing -- unpack first" >&2; exit 1; }
[ -f sonix_player ] || { echo "install_tree: sonix_player is missing" >&2; exit 1; }
[ -d "$BASE" ] || { echo "install_tree: $BASE is missing" >&2; exit 1; }
[ -d "$MODEL" ] || { echo "install_tree: $MODEL is missing" >&2; exit 1; }

# A file is rewritten through a temporary rather than with sed -i, which is
# spelled differently from one sed to the next -- and the temporary is given the
# original's mode before it takes its place. That is not a detail: this script
# rewrites the init script that starts the player, and a rewrite that drops the
# executable bit leaves a firmware whose init silently skips it, which looks
# exactly like a device that hangs on the boot logo.
rewrite() {
	local expression="$1" file="$2"
	local mode
	mode="$(stat -c%a "$file")"
	sed -E "$expression" "$file" > "$file.packer.tmp"
	chmod "$mode" "$file.packer.tmp"
	mv "$file.packer.tmp" "$file"
}

# The stamp and the version string go into system-info.json through rewrite(),
# so they sit in a sed expression the shell itself expands. A backslash, an
# ampersand (the whole match, in a replacement), the | both expressions use as
# their delimiter, a double quote (it would end the JSON string) and a backtick
# (the shell would run what it holds) break the rewrite, and a newline splits it
# in two. Checked here as well as in the packer: this is where the sed runs.
json_safe() {
	case "$2" in
	*$'\n'*)
		echo "install_tree: $1 holds a newline" >&2
		exit 1
		;;
	esac
	if grep -q '[\\&|"`]' <<< "$2"; then
		echo "install_tree: $1 holds a backslash, &, |, a double quote or a backtick" >&2
		exit 1
	fi
}

# ---------------------------------------------------------------------------
# 1. The player, in the stock player's place
# ---------------------------------------------------------------------------
rm -f "$TREE/usr/bin/hiby_player"
install -m 775 sonix_player "$TREE/usr/bin/sonix_player"
echo "    usr/bin/sonix_player installed (775)"

# ---------------------------------------------------------------------------
# 2. The launcher and the init script
# ---------------------------------------------------------------------------
#
# The launcher itself comes from this device's own assets (usr/bin/
# sonix_player.sh, copied in step 3): it is the port's own file, with the stack
# limit, the arena count and sonix_launch in it. The stock launcher goes, since
# nothing starts it any more, and the init script -- which names the launcher --
# is pointed at the new name. One substitution covers both spellings, since
# hiby_player.sh becomes sonix_player.sh.
rm -f "$TREE/usr/bin/hiby_player.sh"
rewrite "s/hiby_player/sonix_player/g" "$TREE/etc/init.d/S92_03_start_music_player"

if grep -q "hiby_player" "$TREE/etc/init.d/S92_03_start_music_player"; then
	echo "install_tree: hiby_player survived the rewrite" >&2
	exit 1
fi
grep -n "sonix_player" "$TREE/etc/init.d/S92_03_start_music_player" | sed 's/^/        /'

# sonix_launch, which the launcher hands over to: the process that starts the
# player and reboots when the player is gone, including when the player cannot
# be exec'd at all. Freestanding, so it depends on nothing in the rootfs.
if [ -f sonix_launch ]; then
	install -m 775 sonix_launch "$TREE/usr/bin/sonix_launch"
else
	echo "install_tree: sonix_launch is missing; the launcher would exec nothing" >&2
	exit 1
fi

# ---------------------------------------------------------------------------
# 3. The assets: the Blaze port's interface, then this device's files over it
# ---------------------------------------------------------------------------
#
# tar rather than cp: it merges into the directories that already exist,
# overwrites what clashes and carries the hidden files, the same way on every
# host. The device's own files go second, so that they win.
tar cf - -C "$BASE" . | tar xf - -C "$TREE"
tar cf - -C "$MODEL" . | tar xf - -C "$TREE"
echo "    $BASE: $(find "$BASE" -type f | wc -l | tr -d ' ') files, then $MODEL over them"

# The build stamp: when the binary in this image was built. The player shows it
# in About and the update check compares it, so it is not decoration -- an image
# whose stamp does not move is an image the player will not offer as an update.
SYSTEM_INFO_JSON="$TREE/usr/resource/sonix/components/system-info.json"
[ -n "${BUILD_STAMP:-}" ] || { echo "install_tree: BUILD_STAMP is empty" >&2; exit 1; }
json_safe BUILD_STAMP "$BUILD_STAMP"
rewrite "s|(\"build_version\"[[:space:]]*:[[:space:]]*\")[^\"]*(\")|\\1$BUILD_STAMP\\2|" "$SYSTEM_INFO_JSON"
grep -q "\"$BUILD_STAMP\"" "$SYSTEM_INFO_JSON" ||
	{ echo "install_tree: build_version was not rewritten" >&2; exit 1; }
echo "    build_version = $BUILD_STAMP"

# And the version string: this build's identity, computed by the packer out of
# this repository -- a tag, a commit's short hash, or an md5 of the uncommitted
# changes (see os_version() in v3_analog_packer.sh). It is the row the player
# shows in About as "Operating system version". Both substitutions are written
# with | rather than / as their delimiter, since the value itself carries a
# slash.
OS_VERSION="${OS_VERSION:-}"
[ -n "$OS_VERSION" ] || { echo "install_tree: OS_VERSION is empty" >&2; exit 1; }
json_safe OS_VERSION "$OS_VERSION"
rewrite "s|(\"OS_version\"[[:space:]]*:[[:space:]]*\")[^\"]*(\")|\\1$OS_VERSION\\2|" "$SYSTEM_INFO_JSON"
grep -q "\"$OS_VERSION\"" "$SYSTEM_INFO_JSON" ||
	{ echo "install_tree: OS_version was not rewritten" >&2; exit 1; }
echo "    OS_version = $OS_VERSION"

# ---------------------------------------------------------------------------
# 4. The stock interface's resources go
# ---------------------------------------------------------------------------
#
# Nothing on the device reads them once the stock player is gone, and they are
# megabytes of an image that has to fit in the flash. usr/resource/sonix -- the
# assets that just landed -- is deliberately not in this list, and neither is
# usr/resource/bt_name, which is the adapter's name and the port reads it.
rm -rf "$TREE/usr/resource/litegui" "$TREE/usr/resource/layout" \
	"$TREE/usr/resource/str" "$TREE/usr/resource/fonts"

# ---------------------------------------------------------------------------
# 5. What the port needs and the stock firmware does not have
# ---------------------------------------------------------------------------
#
# Checked rather than assumed: an image that boots without these looks fine
# until the player is asked for sound over Bluetooth or for a page with words
# on it.
for f in usr/bin/sonix_player usr/bin/bluealsa usr/sbin/bluetoothd \
	usr/lib/libbluetooth.so.3 usr/lib/libfdk-aac.so.2 usr/lib/libopenaptx.so.0 \
	usr/lib/libldacBT_enc.so.2 usr/lib/libldacBT_abr.so.2 \
	usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so \
	usr/share/alsa/alsa.conf.d/20-bluealsa.conf \
	usr/etc/bluetooth/main.conf etc/dbus-1/system.d/sonix-player.conf \
	usr/resource/sonix/fonts/default.otf \
	usr/resource/sonix/components/system-info.json; do
	[ -e "$TREE/$f" ] || { echo "install_tree: $f did not land in the tree" >&2; exit 1; }
done
if [ -f sonix_launch ]; then
	[ -x "$TREE/usr/bin/sonix_launch" ] || { echo "install_tree: usr/bin/sonix_launch is not executable" >&2; exit 1; }
fi
[ -x "$TREE/usr/bin/sonix_launch" ] || { echo "install_tree: usr/bin/sonix_launch is not in the tree" >&2; exit 1; }

# The device's own name, which is what picks the model: the player compares it
# with its table by exact match, and a wrong one leaves it in the stock model's
# mode -- /usr/data instead of /data, HiBy's panel, no keys.
grep -q '"TempoTec V3 Analog"' "$TREE/usr/resource/sonix/components/system-info.json" ||
	{ echo "install_tree: system-info.json does not name the V3 Analog" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 6. Ownership and modes, the way mkfs.ubifs and the boot want them
# ---------------------------------------------------------------------------
#
# The stock tree is 1004:1004 throughout, and mkfs.ubifs writes those numbers
# into the image. The executables need the bit: a launcher that is not
# executable is a player that never starts, which reads as a boot loop.
chown -R 1004:1004 "$TREE"
chmod 775 "$TREE/usr/bin/sonix_player" "$TREE/usr/bin/sonix_player.sh" \
	"$TREE/usr/bin/bluealsa" "$TREE/usr/sbin/bluetoothd" "$TREE/usr/bin/bt_init" \
	"$TREE/etc/init.d/S92_03_start_music_player"
chmod 755 "$TREE/usr/lib/libbluetooth.so.3"
chmod 644 "$TREE/usr/lib/libfdk-aac.so.2" "$TREE/usr/lib/libopenaptx.so.0" \
	"$TREE/usr/lib/libldacBT_enc.so.2" "$TREE/usr/lib/libldacBT_abr.so.2" \
	"$TREE/usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so"

echo "    tree: $(du -sh "$TREE" | cut -f1)"
