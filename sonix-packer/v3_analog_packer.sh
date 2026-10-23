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
#                              somewhere else): the vendor's 1.3 build for this
#                              model, which this repository does not carry --
#                              docs/V3-ANALOG.md, "What a fresh clone needs"
#   sonix_player               the binary to install -- built from sonix-player/
#   sonix_launch               and its launcher when they are not here yet, in
#                              the container sonix-player/docker describes
#   assets/BLAZE/              the TempoTec V3 Blaze's interface assets -- fonts,
#                              languages, images, the Bluetooth userland. Another
#                              player, and this one's base: the interface is
#                              shared, so its resources are installed first
#   assets/V3ANALOG/           and this device's own over them
#   kernel/UIMAGE.BIN          a kernel to install instead of the stock one
#                              (optional, $KERNEL_IMAGE to point elsewhere; the
#                              stock kernel is carried through without it)
#   tools/install_tree.sh      the tree work, run inside the container
#   tools/fix_update_md5.py    the digest the updater checks
#
# and, for the tools themselves (7z, ubireader, mkfs.ubifs, genisoimage), the
# docker image this repository carries the recipe for, which is built on the
# first run:
#
#   sonix-packer/docker
#
# Run it and it does the lot:
#
#   ./v3_analog_packer.sh            # build, then verify the result
#   ./v3_analog_packer.sh verify     # only re-check out/v3_analog.upt
#   STOCK=... ./v3_analog_packer.sh  # from another stock image
#   SKIP_PLAYER_BUILD=1 ./v3_analog_packer.sh   # pack the binaries already here
#   VERSION_OVERRIDE=<base>/<tag> ./v3_analog_packer.sh   # name the build by
#                                    hand, for a release git cannot name here
#
# The host needs docker and nothing else: the player, its toolchain and the
# firmware tools all live in images this repository carries the recipes for.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$SCRIPT_DIR"

STOCK="${STOCK:-$SCRIPT_DIR/v3_analog_original.upt}"
PLAYER_BIN="$SCRIPT_DIR/sonix_player"
LAUNCH_BIN="$SCRIPT_DIR/sonix_launch"
DOCKER_IMAGE="${DOCKER_IMAGE:-tempotec-fw-tools}"

# What this run needs: `verify` only re-checks the image that is already on
# disk, so it builds nothing and asks for nothing beyond docker and that image --
# not the vendor's stock firmware, and not the player.
NEED_PLAYER=1
NEED_STOCK=1

# The stock image's bootloader, which is carried through into ours untouched.
# It is the one file whose contents matter more than anything else here: a wrong
# stock image is a wrong UBOOT.BIN, and writing a wrong bootloader is the one
# way to brick this device. A newer vendor firmware would bring its own digest,
# which is why this is a variable rather than a constant in the check below.
STOCK_UBOOT_MD5="${STOCK_UBOOT_MD5:-1f54858d6b4e6f482a81a3378c0e5bf8}"

# A kernel to install instead of the stock one. Optional: without it the stock
# UIMAGE.BIN is carried through untouched. When it is there its size must not
# grow past the stock image (the kernel partition is sized for the stock), and
# UPDATE.TXT's kernel md5 is recomputed from it -- the updater checks it.
KERNEL_IMAGE="${KERNEL_IMAGE:-$SCRIPT_DIR/kernel/UIMAGE.BIN}"

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
# Warnings go to stderr: they are not part of any value. `os_version` warns when
# the tree is not a git repository, and it is read as `$(os_version)` -- a warning
# on stdout would become part of the version string, and the sed that writes it
# into system-info.json would then fail on it.
warn() { echo -e "${YELLOW}!!${NC}  $*" >&2; }
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
		-e BUILD_STAMP="${BUILD_STAMP:-}" -e OS_VERSION="${OS_VERSION:-}" \
		"$DOCKER_IMAGE" "$@"
}

# When the binary was built, as the stamp the player shows in About. GNU date on
# the host, so the seconds of the file's own mtime are the source.
stamp_of() { date -r "$1" +%d%m%Y%H%M; }

# The version the player shows in About ("Operating system version"). The base is
# the port's own, taken from the asset's system-info.json up to the first '-' or
# '/'; what follows the slash names the build, out of this repository:
#
#   HEAD carries a tag      -> <base>/<tag>          a release
#   a commit without a tag  -> <base>/#<short hash>  that commit, built
#   changes not committed   -> <base>/<8 hex>        a test build; the eight hex
#                              are an md5 of those changes, since the image's own
#                              md5 cannot be written inside the image. The
#                              artefact's name still carries it, as before.
#
# A release whose commit carries no tag -- because the tag lives on another
# branch, or the tree has moved on since -- is built with
# VERSION_OVERRIDE=<base>/<tag> on the command line, which names it by hand
# instead of by git.
os_version() {
	if [ -n "${VERSION_OVERRIDE:-}" ]; then
		printf '%s' "$VERSION_OVERRIDE"
		return 0
	fi

	local repo="$SCRIPT_DIR/.." json base tag
	json="$SCRIPT_DIR/assets/V3ANALOG/usr/resource/sonix/components/system-info.json"
	base="$(sed -nE 's/.*"OS_version"[[:space:]]*:[[:space:]]*"([^"/-]*).*/\1/p' "$json")"
	[ -n "$base" ] || die "no OS_version in $json to take a base from."

	if ! git -C "$repo" rev-parse --git-dir >/dev/null 2>&1; then
		warn "$repo is not a git repository; the version will name no build."
		printf '%s/unknown' "$base"
		return 0
	fi

	if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]; then
		printf '%s/%s' "$base" \
			"$(git -C "$repo" diff HEAD --binary | md5sum | cut -c1-8)"
		return 0
	fi

	tag="$(git -C "$repo" describe --tags --exact-match HEAD 2>/dev/null || true)"
	if [ -n "$tag" ]; then
		printf '%s/%s' "$base" "$tag"
	else
		printf '%s/#%s' "$base" "$(git -C "$repo" rev-parse --short=7 HEAD)"
	fi
}

# Builds the player, in the container its own tree describes, and puts the two
# binaries where this script looks for them. The first run also builds the MIPS
# toolchain, FreeType and the audio libraries -- the 20-60 minutes docs/BUILD.md
# warns about; the runs after it take minutes.
build_player() {
	local player_dir="$SCRIPT_DIR/../sonix-player"
	local wrapper="$player_dir/docker/build.sh"

	[ -x "$wrapper" ] || die "there is no sonix_player next to this script, and no
     $wrapper to build one."

	step "Building the player (a no-op when nothing changed; the first run builds
     the toolchain and takes 20-60 minutes)"
	"$wrapper"
	# -p: the stamp the player shows in About is its build time, and it is read
	# from the file's mtime.
	cp -p "$player_dir/sonix_player" "$player_dir/sonix_launch" "$SCRIPT_DIR/"
}

# The tools image, when it is not there yet. It is built from the recipe in this
# repository, so the host still needs nothing but docker.
build_tools_image() {
	[ -f "$SCRIPT_DIR/docker/Dockerfile" ] ||
		die "no docker image $DOCKER_IMAGE, and no Dockerfile in $SCRIPT_DIR/docker
     to build one from."

	step "Building the tools image $DOCKER_IMAGE (a few minutes, once)"
	docker build -t "$DOCKER_IMAGE" "$SCRIPT_DIR/docker"
}

check_inputs() {
	step "Checking what is needed"

	command -v docker >/dev/null 2>&1 || die "docker is not installed."
	docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1 || build_tools_image

	if [ "$NEED_STOCK" = "1" ]; then
		[ -f "$STOCK" ] || die "no stock image at $STOCK (set STOCK=...): the vendor's 1.3
     firmware for the V3 Analog, which this repository does not carry. See
     docs/V3-ANALOG.md, 'What a fresh clone needs', for what it has to be."
		# The build directory is wiped before the stock image is unpacked, so a
		# stock image kept inside it would be deleted before it was read.
		case "$(cd "$(dirname "$STOCK")" && pwd)/$(basename "$STOCK")" in
		"$BUILD"/*) die "the stock image is inside $BUILD, which this script clears:
     copy it somewhere else and pass that path (STOCK=...)." ;;
		esac
	fi

	# The player is not carried in the repository either -- it is built from its
	# sources, in the container its own Dockerfile describes. Asking for it
	# instead of building it would make the one documented entry point depend on
	# a step the reader has to know about; this way `STOCK=... ./v3_analog_packer.sh`
	# is the whole recipe on a machine with nothing but docker.
	#
	# Asked every run, not only when the binaries are missing: `make` decides
	# what is stale in a second when nothing changed, and a binary left from an
	# earlier build would otherwise be packed into an image that claims to be
	# this commit's. SKIP_PLAYER_BUILD=1 packs what is here, for a run that only
	# wants to repack.
	if [ "$NEED_PLAYER" = "1" ] && [ -z "${SKIP_PLAYER_BUILD:-}" ]; then
		build_player
	fi

	if [ "$NEED_PLAYER" = "1" ]; then
		[ -f "$PLAYER_BIN" ] || die "no sonix_player next to this script: build it
     with sonix-player/docker/build.sh, or run this script without
     SKIP_PLAYER_BUILD=1."
		[ -f "$LAUNCH_BIN" ] || die "no sonix_launch next to this script: build it
     with sonix-player/docker/build.sh, or run this script without
     SKIP_PLAYER_BUILD=1."

		[ -d assets/BLAZE ] || die "assets/BLAZE is missing."
		[ -d assets/V3ANALOG ] || die "assets/V3ANALOG is missing."

		BUILD_STAMP="$(stamp_of "$PLAYER_BIN")"
		OS_VERSION="$(os_version)"
	fi

	if [ "$NEED_STOCK" = "1" ]; then
		say "    stock:  $STOCK ($(stat -c%s "$STOCK") bytes)"
	fi
	if [ "$NEED_PLAYER" = "1" ]; then
		say "    player: $(stat -c%s "$PLAYER_BIN") bytes, built $BUILD_STAMP"
		say "    version: OS_version $OS_VERSION"
		say "    launch: $(stat -c%s "$LAUNCH_BIN") bytes"
		if [ -f "$KERNEL_IMAGE" ]; then
			say "    kernel: $(stat -c%s "$KERNEL_IMAGE") bytes, md5 $(md5sum "$KERNEL_IMAGE" | cut -d' ' -f1) -- installed instead of the stock one"
		else
			say "    kernel: the stock UIMAGE.BIN, unchanged"
		fi
	fi
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

	# The bootloader is carried through, so the stock image had better be the
	# stock image: this is the one file that bricks the device when it is wrong,
	# and it is not the file the rest of the build would notice.
	stock_uboot_md5="$(md5sum "$ISO_DIR/UBOOT.BIN" | cut -d' ' -f1)"
	if [ "$stock_uboot_md5" != "$STOCK_UBOOT_MD5" ]; then
		die "this stock image's UBOOT.BIN is $stock_uboot_md5, not the V3 Analog stock
     bootloader $STOCK_UBOOT_MD5. Either it is the wrong model's firmware, or a
     vendor version newer than the one this port was built on -- in which case
     read what changed, then set STOCK_UBOOT_MD5=$stock_uboot_md5 to say so."
	fi
	say "    UBOOT.BIN: $stock_uboot_md5, the stock bootloader (carried through)"

	fw ubireader_extract_files -k -o "build/SYSTEM" "build/iso/SYSTEM.UBI"
	[ -d "$TREE/usr/bin" ] || die "the rootfs did not extract."
	say "    rootfs: $(fw du -sh build/SYSTEM | cut -f1)"

	if [ -f "$KERNEL_IMAGE" ]; then
		stock_size="$(stat -c%s "$ISO_DIR/UIMAGE.BIN")"
		new_size="$(stat -c%s "$KERNEL_IMAGE")"
		[ "$new_size" -le "$stock_size" ] ||
			die "kernel/UIMAGE.BIN is $new_size bytes, the stock kernel $stock_size: it may not grow past the stock image."
		cp -f "$KERNEL_IMAGE" "$ISO_DIR/UIMAGE.BIN"
		say "    kernel: replaced with $new_size bytes (stock $stock_size)"
	fi
	say ""
}

install_tree() {
	step "Installing the player and the assets"
	fw bash tools/install_tree.sh
	say ""
}

trim_stock() {
	step "Trimming the stock libraries nothing uses"

	# Not tidiness: this rootfs boots 100% full, and a test build replaces a
	# 7.4 MB binary in place -- on a full volume that copy dies halfway and
	# leaves a player that cannot start. ~6 MB of the stock player's decoders
	# and a duplicate libcrypto are what buys that headroom back.
	#
	# The script itself fails the build if a remaining binary still needs one of
	# the files it removed, so this is not a leap of faith.
	fw python3 tools/trim_stock.py
	say ""
}

pack_image() {
	step "Packing the image"

	# -c 480 is the rootfs volume's size in LEBs on this device; mkfs.ubifs
	# refuses a tree that does not fit, which is the check that matters here.
	fw mkfs.ubifs -x lzo -d build/SYSTEM -e 0x1F000 -c 480 -m 0x800 -o build/iso/SYSTEM.UBI ||
		die "the rootfs does not fit in the volume (too many files?)."

	fw python3 tools/fix_update_md5.py build/iso/UPDATE.TXT build/iso/SYSTEM.UBI build/iso/UIMAGE.BIN

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

def md5_of(path):
    digest = hashlib.md5()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()

ok = True
for block_name, image in (("rootfs", "SYSTEM.UBI"), ("kernel", "UIMAGE.BIN")):
    block = text[text.find(f"{block_name}={{"):]
    declared = re.search(r"md5=([0-9a-fA-F]{32})", block).group(1)
    found = md5_of(iso / image)
    print(f"    {block_name} md5: UPDATE.TXT {declared}, image {found}")
    ok = ok and declared == found
sys.exit(0 if ok else 1)
PY

	fw ubireader_extract_files -k -o "build/verify/SYSTEM" "build/verify/SYSTEM.UBI"
	# Relative: inside the container this directory is /packer, so an absolute
	# host path names nothing there.
	VT="build/verify/SYSTEM"

	# What the player will report in About, read back out of the image.
	say "    OS_version: $(sed -nE 's/.*"OS_version"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p' \
		"$VT/usr/resource/sonix/components/system-info.json")"

	# Back to the host before anything can fail: a tree left owned by root is a
	# tree the next run cannot clean up.
	docker run --rm -i -v "$SCRIPT_DIR:/packer" -w /packer "$DOCKER_IMAGE" \
		chown -R "$(id -u):$(id -g)" build >/dev/null 2>&1 || true

	fw python3 - "$VT" <<'PY'
import os, re, stat, sys
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

# The D-Bus policy has to be valid XML: dbus-daemon drops a malformed file
# whole, silently, and a double hyphen inside an XML comment is enough to make
# it malformed. That is how this port shipped a firmware in which bluealsa
# could not own its name, so nothing A2DP worked at all.
policy = tree / "etc/dbus-1/system.d/sonix-player.conf"
if policy.exists():
    import xml.dom.minidom
    try:
        xml.dom.minidom.parse(str(policy))
    except Exception as exc:
        problems.append(f"the D-Bus policy is not valid XML: {exc}")
    if 'own="org.bluealsa"' not in policy.read_text():
        problems.append("the D-Bus policy does not let bluealsa own its name")

launcher = tree / "usr/bin/sonix_player.sh"
if launcher.exists() and re.search(r"^\s*(?:export\s+)?SONIX_LOG[ \t]*=", launcher.read_text(), re.M):
    problems.append("the launcher forces SONIX_LOG, which overrides the log setting")

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
	trim_stock
	pack_image
	verify_image
	;;
verify)
	NEED_PLAYER=0
	NEED_STOCK=0
	check_inputs
	verify_image
	;;
*)
	echo "usage: $0 [all|verify]" >&2
	exit 1
	;;
esac
