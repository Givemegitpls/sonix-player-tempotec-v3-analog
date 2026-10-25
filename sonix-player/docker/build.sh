#!/bin/bash
#
# Builds the player in the toolchain image, so that the machine needs docker and
# nothing else. The image is sonix-player/docker/Dockerfile; it is built on the
# first run (a few minutes, once).
#
#   docker/build.sh              make target -j<nproc>
#   docker/build.sh clean        wipe build_*, keep the toolchain and libraries
#   docker/build.sh target -j2   anything else `make` takes, passed through
#
# `host` is not here: the SDL simulator needs headers this image does not carry
# (SDL2, FreeType, opusfile, wavpack, ALSA). Build it on the machine, `make host`
# in the tree -- see docs/BUILD.md.
#
# Two things make this the same build the host would do:
#
#   - the tree is mounted at the absolute path it has on the host. The build's
#     .d and .o files carry paths, and the compiler's own messages name them; if
#     the container saw /work instead, every switch between the two would
#     rebuild the whole tree.
#   - it runs as the calling user, so nothing in the tree ends up owned by root.
#
# The outputs are the host's: rockbox-toolchain/, freetype-target/,
# audio-target/ and build_target/ are written into the mounted tree. The first
# run builds the toolchain and the two library prefixes (20-60 minutes); every
# run after it goes straight to the player.
#
# The simulator needs SDL2, FreeType, opusfile, wavpack and ALSA headers, which
# the image does not carry: `host` here is for a tree whose host-side libraries
# are already installed, and `make host` on the machine does the same thing.
#
set -euo pipefail

IMAGE="${IMAGE:-tempotec-player-build}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
PLAYER_DIR="$(dirname "$HERE")"
REPO="$(dirname "$PLAYER_DIR")"

command -v docker >/dev/null 2>&1 || {
	echo "build.sh: docker is not installed, and this is what builds the player" >&2
	exit 1
}

# Asked every run, not only when the image is missing: docker reuses the layers
# of an unchanged Dockerfile in a second, and an edit to the package list is
# otherwise silently left out of the image -- which fails much later, inside the
# build, as a missing tool.
echo "build.sh: the image $IMAGE (cached layers, a few minutes the first time)"
docker build -t "$IMAGE" "$HERE"

# A toolchain that already exists elsewhere is reused rather than rebuilt: the
# Makefile's variable is what it looks for, and the directory is mounted into
# the container because it lives outside the tree.
RUN_EXTRA=()
TOOLCHAIN="$PLAYER_DIR/rockbox-toolchain"
if [ -n "${ROCKBOX_TOOLCHAIN_DIR:-}" ]; then
	[ -x "$ROCKBOX_TOOLCHAIN_DIR/bin/mipsel-rockbox-linux-gnu-gcc" ] || {
		echo "build.sh: ROCKBOX_TOOLCHAIN_DIR=$ROCKBOX_TOOLCHAIN_DIR has no mipsel-rockbox-linux-gnu-gcc" >&2
		exit 1
	}
	RUN_EXTRA+=(-v "$ROCKBOX_TOOLCHAIN_DIR:$ROCKBOX_TOOLCHAIN_DIR:ro"
		-e ROCKBOX_TOOLCHAIN_DIR="$ROCKBOX_TOOLCHAIN_DIR")
	TOOLCHAIN="$ROCKBOX_TOOLCHAIN_DIR"
fi

# A toolchain left behind by an interrupted run is worse than none. The
# Makefile's rule for it is a file target -- the compiler binary -- and that
# binary is built before the C library it links against, so an interrupted
# rockboxdev.sh leaves a compiler that make calls done and every configure that
# follows then fails with "C compiler cannot create executables". libc and
# libasound are what the player links; both are what says the toolchain is whole.
# A toolchain from outside the tree is the caller's to look after.
SYSROOT="$TOOLCHAIN/mipsel-rockbox-linux-gnu/sysroot"
if [ -z "${ROCKBOX_TOOLCHAIN_DIR:-}" ] && [ -x "$TOOLCHAIN/bin/mipsel-rockbox-linux-gnu-gcc" ] &&
	{ [ ! -f "$SYSROOT/lib/libc.so.6" ] || [ ! -f "$SYSROOT/usr/lib/libasound.so" ]; }; then
	echo "build.sh: $TOOLCHAIN has a compiler but not the libraries the player links, so the toolchain build was interrupted; removing it and building it again" >&2
	rm -rf "$TOOLCHAIN"
fi

target="${1:-target}"
[ $# -gt 0 ] && shift
if [ "$target" = "target" ] && [ $# -eq 0 ]; then
	set -- "-j$(nproc)"
fi

# HOME is the container's /tmp: wget and git want one, and the calling user has
# no passwd entry inside the image. RUN_EXTRA carries the toolchain from outside
# the tree -- its mount and the variable that names it, without which the
# Makefile would build a second one into the tree.
exec docker run --rm \
	-v "$REPO:$REPO" \
	-w "$PLAYER_DIR" \
	--user "$(id -u):$(id -g)" \
	-e HOME=/tmp \
	"${RUN_EXTRA[@]}" \
	"$IMAGE" \
	make "$target" "$@"
