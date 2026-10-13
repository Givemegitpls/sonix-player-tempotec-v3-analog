#!/bin/bash
#
# Builds a v3_analog.upt that boots Sonix Player on the TempoTec V3 Analog.
#
# The stock image is the starting point, not a layer: this unpacks it, installs
# the player and its assets in place of the stock player and the stock
# interface's resources, and packs the result back into the same shape -- an
# ISO with SYSTEM.UBI (the UBIFS rootfs), UIMAGE.BIN, UBOOT.BIN and the
# UPDATE.TXT that has to name the new rootfs's digest.
#
# UBOOT.BIN is carried through untouched, on purpose: it is the bootloader, and
# the way to flash this device is the recovery kernel (Power + Prev with
# update.upt on the card), not a bootloader write.
#
# Everything it needs sits next to it:
#
#   v3_analog_packer.sh        this script
#   v3_analog_original.upt     the stock firmware to start from ($STOCK to point
#                              somewhere else; the sibling project keeps one at
#                              ../tempotec v3/v3_analog.upt)
#   sonix_player               the binary to install (make target in sonix-player/)
#   assets/BLAZE/              the player's own interface assets: fonts,
#                              languages, images, and the Bluetooth userland
#   assets/V3ANALOG/           and what is this device's own, over them
#   tools/install_tree.sh      the tree work, run inside the container
#   tools/fix_update_md5.py    the digest the updater checks
#
# and, for the tools themselves (7z, ubireader, mkfs.ubifs, genisoimage), the
# docker image the sibling project builds:
#
#   docker build -t tempotec-fw-tools "../tempotec v3/docker"
#
# Run it and it does the lot:
#
#   ./v3_analog_packer.sh            # build, then verify the result
#   ./v3_analog_packer.sh verify     # only re-check out/v3_analog.upt
#   STOCK=... ./v3_analog_packer.sh  # from another stock image
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$SCRIPT_DIR"

STOCK="${STOCK:-$SCRIPT_DIR/v3_analog_original.upt}"
PLAYER_BIN="$SCRIPT_DIR/sonix_player"
LAUNCH_BIN="$SCRIPT_DIR/sonix_launch"
DOCKER_IMAGE="${DOCKER_IMAGE:-tempotec-fw-tools}"

BUILD="$SCRIPT_DIR/build"
ISO_DIR="$BUILD/iso"	 # the stock ISO, unpacked
TREE="$BUILD/SYSTEM"	 # and its rootfs, extracted
VERIFY_DIR="$BUILD/verify"
OUT_DIR="$SCRIPT_DIR/out"
OUT="$OUT_DIR/v3_analog.upt"

RED='\033[1;31m'
GREEN='\033[1;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

say() { echo -e "$@"; }
step() { echo -e "${BLUE}==>${NC} $*"; }
warn() { echo -e "${YELLOW}!!${NC}  $*"; }
die() {
	echo -e "${RED}Error:${NC} $*" >&2
	exit 1
}

# The tools live in a container, which is mounted this directory as /packer and
# runs as root: ubireader has to create symlinks and device nodes in the tree,
# and mkfs.ubifs writes uid/gid into the image. The build stamp travels in
# because it is the host that knows when the binary was built.
fw() {
	docker run --rm -i -v "$SCRIPT_DIR:/packer" -w /packer \
		-e BUILD_STAMP="${BUILD_STAMP:-}" "$DOCKER_IMAGE" "$@"
}

# When the binary was built, as the stamp the player shows in About. GNU date on
# the host, so the seconds of the file's own mtime are the source.
stamp_of() { date -r "$1" +%d%m%Y%H%M; }

check_inputs() {
	step "Checking what is needed"

	command -v docker >/dev/null 2>&1 || die "docker is not installed."
	docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1 ||
		die "the docker image $DOCKER_IMAGE is not built. See the header of this script."

	[ -f "$STOCK" ] || die "no stock image at $STOCK (set STOCK=...)."
	[ -f "$PLAYER_BIN" ] || die "no sonix_player next to this script: build it with 'make target' in sonix-player/."
	[ -d assets/BLAZE ] || die "assets/BLAZE is missing."
	[ -d assets/V3ANALOG ] || die "assets/V3ANALOG is missing."

	BUILD_STAMP="$(stamp_of "$PLAYER_BIN")"
	say "    stock:  $STOCK ($(stat -c%s "$STOCK") bytes)"
	say "    player: $(stat -c%s "$PLAYER_BIN") bytes, built $BUILD_STAMP"
	[ -f "$LAUNCH_BIN" ] || die "no sonix_launch next to this script: build it with 'make target' in sonix-player/."
	say "    launch: $(stat -c%s "$LAUNCH_BIN") bytes"
	say ""
}

unpack_stock() {
	step "Unpacking the stock image"

	# The trees of a previous run belong to root (the container writes them), so
	# the removal happens inside the container too: on the host it would fail on
	# every file it does not own.
	fw rm -rf build
	mkdir -p "$ISO_DIR" "$OUT_DIR"
	cp -f "$STOCK" "$BUILD/stock.upt"

	fw 7z x -y "build/stock.upt" -o"build/iso" >/dev/null

	for f in SYSTEM.UBI UIMAGE.BIN UBOOT.BIN UPDATE.TXT VERSION.TXT _GITIGNO; do
		[ -f "$ISO_DIR/$f" ] || die "the stock image has no $f: not a firmware of this kind."
	done
	say "    ISO: SYSTEM.UBI, UIMAGE.BIN, UBOOT.BIN, UPDATE.TXT, VERSION.TXT, _GITIGNO"

	fw ubireader_extract_files -k -o "build/SYSTEM" "build/iso/SYSTEM.UBI"
	[ -d "$TREE/usr/bin" ] || die "the rootfs did not extract."
	say "    rootfs: $(fw du -sh build/SYSTEM | cut -f1)"
	say ""
}

install_tree() {
	step "Installing the player and the assets"
	fw bash tools/install_tree.sh
	say ""
}

pack_image() {
	step "Packing the image"

	# -c 480 is the rootfs volume's size in LEBs on this device; mkfs.ubifs
	# refuses a tree that does not fit, which is the check that matters here.
	fw mkfs.ubifs -x lzo -d build/SYSTEM -e 0x1F000 -c 480 -m 0x800 -o build/iso/SYSTEM.UBI ||
		die "the rootfs does not fit in the volume (too many files?)."

	fw python3 tools/fix_update_md5.py build/iso/UPDATE.TXT build/iso/SYSTEM.UBI

	fw genisoimage -l -o out/v3_analog.upt \
		build/iso/SYSTEM.UBI build/iso/UIMAGE.BIN build/iso/UBOOT.BIN \
		build/iso/UPDATE.TXT build/iso/VERSION.TXT build/iso/_GITIGNO

	# The container writes as root.
	docker run --rm -i -v "$SCRIPT_DIR:/packer" -w /packer "$DOCKER_IMAGE" \
		chown -R "$(id -u):$(id -g)" build out >/dev/null 2>&1 || true

	say "    image: $OUT ($(stat -c%s "$OUT") bytes)"
	say "    UPDATE.TXT:"; sed 's/^/        /' "$ISO_DIR/UPDATE.TXT"
	say ""
}

verify_image() {
	step "Verifying the image"

	[ -f "$OUT" ] || die "no image at $OUT: build it first."

	# Root-owned from the build, so removed and re-made in the container.
	fw rm -rf build/verify
	mkdir -p "$VERIFY_DIR"
	fw 7z x -y "out/v3_analog.upt" -o"build/verify" >/dev/null

	# The digest the updater checks, recomputed from what is inside the ISO.
	fw python3 - <<'PY'
import hashlib, re, sys
from pathlib import Path

iso = Path("build/verify")
text = (iso / "UPDATE.TXT").read_text()
block = text[text.find("rootfs={"):]
declared = re.search(r"md5=([0-9a-fA-F]{32})", block).group(1)

digest = hashlib.md5()
with (iso / "SYSTEM.UBI").open("rb") as fh:
    for chunk in iter(lambda: fh.read(1 << 20), b""):
        digest.update(chunk)
found = digest.hexdigest()

print(f"    rootfs md5: UPDATE.TXT {declared}, image {found}")
sys.exit(0 if declared == found else 1)
PY

	fw ubireader_extract_files -k -o "build/verify/SYSTEM" "build/verify/SYSTEM.UBI"
	# Relative: inside the container this directory is /packer, so an absolute
	# host path names nothing there.
	VT="build/verify/SYSTEM"

	# Back to the host before anything can fail: a tree left owned by root is a
	# tree the next run cannot clean up.
	docker run --rm -i -v "$SCRIPT_DIR:/packer" -w /packer "$DOCKER_IMAGE" \
		chown -R "$(id -u):$(id -g)" build >/dev/null 2>&1 || true

	fw python3 - "$VT" <<'PY'
import os, stat, sys
from pathlib import Path

tree = Path(sys.argv[1])
problems = []

def want(rel, executable=None):
    path = tree / rel
    if not path.exists():
        problems.append(f"{rel} is missing")
        return
    if executable is not None:
        mode = stat.S_IMODE(path.stat().st_mode)
        if bool(mode & 0o111) != executable:
            problems.append(f"{rel} is mode {mode:o}")

want("usr/bin/sonix_player", True)
want("usr/bin/sonix_player.sh", True)
want("usr/bin/sonix_launch", True)
# The init script that starts the launcher: without the bit, init skips it and
# the device sits on the boot logo with nothing running.
want("etc/init.d/S92_03_start_music_player", True)
# The adb watchdog: executable, or it cannot keep the device reachable when the
# player does not come up.
want("etc/init.d/S98adb_debug", True)
# The script the player's ADB switch calls on a legacy-gadget firmware.
want("etc/init.d/adb/S310adb", True)
want("usr/bin/bluealsa", True)
want("usr/sbin/bluetoothd", True)
want("usr/bin/bt_init", True)
want("usr/lib/libbluetooth.so.3")
want("usr/lib/libfdk-aac.so.2")
want("usr/lib/libopenaptx.so.0")
want("usr/lib/libldacBT_enc.so.2")
want("usr/lib/libldacBT_abr.so.2")
want("usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so")
want("usr/share/alsa/alsa.conf.d/20-bluealsa.conf")
want("usr/etc/bluetooth/main.conf")
want("etc/dbus-1/system.d/sonix-player.conf")
want("usr/resource/sonix/fonts/default.otf")
want("usr/resource/sonix/components/system-info.json")

if (tree / "usr/bin/hiby_player").exists():
    problems.append("the stock player is still in the image")
for gone in ("usr/resource/litegui", "usr/resource/layout", "usr/resource/str", "usr/resource/fonts"):
    if (tree / gone).exists():
        problems.append(f"{gone} should have been left out")

info = tree / "usr/resource/sonix/components/system-info.json"
if info.exists() and '"TempoTec V3 Analog"' not in info.read_text():
    problems.append("system-info.json does not name the V3 Analog")

launcher = tree / "usr/bin/sonix_player.sh"
if launcher.exists() and "SONIX_LOG" not in launcher.read_text():
    problems.append("the launcher does not set SONIX_LOG")

bt_init = tree / "usr/bin/bt_init"
if bt_init.exists() and "brcm_patchram_plus" not in bt_init.read_text():
    problems.append("usr/bin/bt_init does not attach the controller")

# bluealsa and bluetoothd have to be the ones this image carries, not the
# stock's: the stock bluetoothd is BlueZ 4 and does not speak to bluealsa.
for rel, other in (("usr/sbin/bluetoothd", "assets/V3ANALOG/usr/sbin/bluetoothd"),
                   ("usr/bin/bluealsa", "assets/V3ANALOG/usr/bin/bluealsa")):
    a, b = tree / rel, Path(other)
    if a.exists() and b.exists() and a.read_bytes() != b.read_bytes():
        problems.append(f"{rel} is not the one the assets carry")

if problems:
    print("    PROBLEMS:")
    for p in problems:
        print(f"      - {p}")
    sys.exit(1)
print("    tree: the player, the launcher, the assets and the Bluetooth userland are in place")
PY

	say ""
	say "${GREEN}=== ok${NC}: $OUT"
	say ""
	say "To flash it: copy it to the card as ${YELLOW}v3_analog.upt${NC} and enter recovery"
	say "(Power + Prev), then choose the update. UBOOT.BIN is the stock one, untouched."
}

case "${1:-all}" in
all)
	say "${BLUE}###  Sonix Player -- V3 Analog firmware packer  ###${NC}"
	say ""
	check_inputs
	unpack_stock
	install_tree
	pack_image
	verify_image
	;;
verify)
	check_inputs
	verify_image
	;;
*)
	echo "usage: $0 [all|verify]" >&2
	exit 1
	;;
esac
