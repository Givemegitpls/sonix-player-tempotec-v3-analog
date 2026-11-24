#!/bin/bash
#
# Runs inside `tempotec-wasm-tools` (see ../docker/wasm/Dockerfile) with:
#
#   /support   this directory, read-only
#   /work      sonix-packer/build/wasm, writable: downloads, extracted sources,
#              and out/
#
# Fetches mtd-utils, lzo and zlib by URL with a pinned sha256, compiles the
# sources `mkfs.ubifs` is made of, links them with the shim in src/exports.c and
# writes out/mkfs_ubifs.js + out/mkfs_ubifs.wasm.
#
# Why these versions: mtd-utils 2.2.0 is what Ubuntu 24.04 installs, which is what
# the packer's own tools image carries -- so the browser and the packer run the
# same mkfs.ubifs. lzo 2.10 and zlib 1.3.2 are the same libraries a host build
# would have linked.

set -euo pipefail

MTD_URL=http://archive.ubuntu.com/ubuntu/pool/main/m/mtd-utils/mtd-utils_2.2.0.orig.tar.xz
MTD_SHA=1878183e7a66bd81bcdb647e50ca0e4238571356c3ba327a51caf68b0b77aeef
LZO_URL=http://archive.ubuntu.com/ubuntu/pool/main/l/lzo2/lzo2_2.10.orig.tar.gz
LZO_SHA=c0f892943208266f9b6543b3ae308fab6284c5c90e627931446fb49b4221a072
ZLIB_URL='http://archive.ubuntu.com/ubuntu/pool/main/z/zlib/zlib_1.3.dfsg+really1.3.2.orig.tar.gz'
ZLIB_SHA=7b6903eb019983987b7112eccf90f1703f1c6c0e0cede36564bf611d19ca579d

DL=/work/dl
SRC=/work/src
OBJ=/work/obj
OUT=/work/out
SUPPORT=/support

mkdir -p "$DL" "$SRC" "$OBJ" "$OUT"

fetch() {
	local url="$1" name="$2" sha="$3"
	if [ ! -f "$DL/$name" ]; then
		echo "    fetching $name"
		curl -fsS --retry 3 --max-time 300 -o "$DL/$name.part" "$url"
		mv "$DL/$name.part" "$DL/$name"
	fi
	echo "$sha  $DL/$name" | sha256sum -c --status ||
		{ echo "mkfs_ubifs wasm: $name does not match its sha256" >&2; exit 1; }
}

unpack() {
	local name="$1" dir="$2" tarball="$3"
	[ -d "$SRC/$dir" ] && return
	echo "    extracting $name"
	mkdir -p "$SRC/$name"
	tar -xf "$DL/$tarball" -C "$SRC/$name" --strip-components=1
	mv "$SRC/$name" "$SRC/$dir"
}

echo "==> sources"
fetch "$MTD_URL" mtd-utils-2.2.0.tar.xz "$MTD_SHA"
fetch "$LZO_URL" lzo2-2.10.tar.gz "$LZO_SHA"
fetch "$ZLIB_URL" zlib-1.3.2.tar.gz "$ZLIB_SHA"
unpack mtd-utils mtd-utils-2.2.0 mtd-utils-2.2.0.tar.xz
unpack lzo lzo-2.10 lzo2-2.10.tar.gz
unpack zlib zlib-1.3.2 zlib-1.3.2.tar.gz

CC=emcc
# -include time.h: mkfs.ubifs.c calls time() without including <time.h>, which
# glibc's headers make up for and musl's do not.
# WITH_LZO/WITH_ZLIB: mtd-utils' configure sets these when the libraries are
# found, and compr.c compiles the corresponding compressors only under them --
# without them `-x lzo` is "bad compressor name".
CFLAGS=(-O2 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -DWITH_LZO -DWITH_ZLIB -include time.h
	-I"$SUPPORT/include" -I"$SRC/mtd-utils-2.2.0/include")
MKFS_SRC="$SRC/mtd-utils-2.2.0/ubifs-utils/mkfs.ubifs"
CFLAGS+=(-I"$MKFS_SRC" -I"$SRC/lzo-2.10/include" -I"$SRC/lzo-2.10/src"
	-I"$SRC/zlib-1.3.2")

echo "==> mtd-utils: mkfs.ubifs"
# The objects of the mkfs.ubifs target, from ubifs-utils/Makemodule.am, without
# the crypto/sign half (no --with-crypto, no keys in a browser build).
# -Dmain=mkfs_ubifs_main: Debian's clang renames a wasm `main` to
# __main_argc_argv (LLVM's wasm entry convention), which is not the symbol the
# linker and emscripten's runtime look for. Under a name of our own the entry
# point survives, and the JavaScript glue calls it with its own argv.
# -Dioctl=wasm_ioctl_stub: the only ioctls mkfs.ubifs makes are FS_IOC_GETFLAGS,
# and emscripten's ioctl syscall aborts on requests it does not know. The stub in
# src/exports.c answers ENOTTY, which the tool reads as "no flags".
MKFS_CFLAGS=("${CFLAGS[@]}" -DVERSION='"2.2.0-wasm"' -Dmain=mkfs_ubifs_main -Dioctl=wasm_ioctl_stub)
for f in mkfs.ubifs crc16 lpt compr devtable \
	hashtable/hashtable hashtable/hashtable_itr; do
	$CC "${MKFS_CFLAGS[@]}" -c "$MKFS_SRC/$f.c" -o "$OBJ/mkfs_$(basename "$f").o"
done

echo "==> mtd-utils: libubi/libmtd, which the mkfs.ubifs target links against"
# Its LDADD names libmtd.a libubi.a (they carry the ioctl wrappers the tool
# references on the paths that talk to a device node). No device is ever opened
# in the browser; the objects only have to exist for the link.
MTD_LIB="$SRC/mtd-utils-2.2.0/lib"
for f in libubi libmtd libcrc32 common libmtd_legacy libubigen; do
	$CC "${CFLAGS[@]}" -c "$MTD_LIB/$f.c" -o "$OBJ/mtdlib_$f.o"
done

echo "==> lzo: the compressor mkfs.ubifs writes with, the decompressor the reader uses"
for f in lzo_init lzo1x_d1 lzo1x_d2 lzo1x_1 lzo1x_9x lzo_ptr lzo_util lzo_crc lzo_str; do
	$CC "${CFLAGS[@]}" -c "$SRC/lzo-2.10/src/$f.c" -o "$OBJ/lzo_$f.o"
done

echo "==> zlib: mkfs.ubifs' other compressor"
for f in adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil; do
	$CC "${CFLAGS[@]}" -I"$SRC/zlib-1.3.2" -c "$SRC/zlib-1.3.2/$f.c" -o "$OBJ/zlib_$f.o"
done

echo "==> shims: uuid, and the LZO entry points JavaScript calls"
$CC "${CFLAGS[@]}" -c "$SUPPORT/src/uuid.c" -o "$OBJ/uuid.o"
$CC "${CFLAGS[@]}" -c "$SUPPORT/src/exports.c" -o "$OBJ/exports.o"

echo "==> linking"
# INVOKE_RUN=0: the module is loaded, then main() is called per build with the
# tree mounted; MODULARIZE/EXPORT_ES6: one factory, importable from a worker.
# MEMORY: the stock rootfs is a 69 MB tree and the image it is packed into is
# 47 MB, both of which pass through the wasm heap.
# ENVIRONMENT=web,worker: emscripten 3.1.6's node loader is written for CommonJS
# and reads __dirname, which an ES module has not got -- a Node harness hands the
# module its wasmBinary instead and this code path never runs.
$CC -O2 \
	-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createMkfsUbifs \
	-sENVIRONMENT=web,worker \
	-sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sMAXIMUM_MEMORY=4294967296 \
	-sFORCE_FILESYSTEM=1 -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 \
	-sEXPORTED_FUNCTIONS=_mkfs_ubifs_main,_malloc,_free,_wasm_lzo_init,_wasm_lzo1x_decompress_safe,_wasm_ioctl_stub \
	-sEXPORTED_RUNTIME_METHODS=FS,HEAPU8,HEAPU32,stringToUTF8,lengthBytesUTF8,UTF8ToString \
	-o "$OUT/mkfs_ubifs.js" "$OBJ"/*.o

echo "==> done"
ls -l "$OUT"
