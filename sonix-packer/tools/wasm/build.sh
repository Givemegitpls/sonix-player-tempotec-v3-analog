#!/bin/bash
#
# Builds mkfs_ubifs.wasm + mkfs_ubifs.js: the UBIFS writer the browser firmware
# page runs, plus the LZO decompressor it reads the vendor's image with.
#
# Run on the host; everything happens in the image `sonix-packer/docker/wasm`
# describes, the same way the packer's own tools do (see Dockerfile there).
#
#   tools/wasm/build.sh            # build, writing build/wasm/out/
#   tools/wasm/build.sh --clean    # drop the download/extract cache first
#
# The result (git-ignored, like every other build output):
#
#   sonix-packer/build/wasm/out/mkfs_ubifs.js    the ESM loader (--emit-modular)
#   sonix-packer/build/wasm/out/mkfs_ubifs.wasm  the module itself
#
# It is served to the page by .github/workflows/pages.yml, which builds it on
# every deployment; nothing here is committed as a binary.
#
# Source tarballs come from Ubuntu's pool, pinned by sha256 in inside.sh, and are
# cached in build/wasm/dl so a second run needs no network.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
PACKER_DIR="$(cd "$SCRIPT_DIR/../.." >/dev/null 2>&1 && pwd)"
IMAGE="${WASM_IMAGE:-tempotec-wasm-tools}"
WORK="$PACKER_DIR/build-wasm"

if [ "${1:-}" = "--clean" ]; then
	rm -rf "$WORK/dl" "$WORK/src" "$WORK/out"
	echo "==> cleared $WORK"
fi

mkdir -p "$WORK/dl" "$WORK/out" "$WORK/src"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "==> building $IMAGE (once; apt + emscripten, a few minutes)"
	# --network host: the default bridge cannot resolve DNS on this host
	# (docs/BUILD.md has the same note for the other images).
	docker build --network host -t "$IMAGE" "$PACKER_DIR/docker/wasm"
fi

echo "==> compiling mkfs.ubifs for wasm32"
docker run --rm --network host \
	-v "$SCRIPT_DIR:/support:ro" \
	-v "$WORK:/work" \
	-w /work "$IMAGE" bash /support/inside.sh

ls -l "$WORK/out"
