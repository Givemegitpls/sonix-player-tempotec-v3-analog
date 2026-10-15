#!/bin/bash
#
# build.sh -- cross-build bluealsa (bluez-alsa 4.0.0 + the --io-rt-priority
# option backported from 4.2.0) for the TempoTec V3 Analog (Ingenic X1000,
# MIPS32r2, glibc 2.16).
#
# Everything is linked against the *device's own* libraries (glib 2.46.1,
# dbus 1.10, alsa 1.0.26) -- the same trick the project uses for glibc -- so
# the binaries load on the device without shipping a second libc/userland.
# Headers of suitable versions come from Debian's mipsel packages.
#
# Outputs (in ./out):
#   bluealsa                                        -> /usr/bin/bluealsa
#   libasound_module_pcm_bluealsa.so                -> /usr/lib/alsa-lib/
#
# Usage:  ./build.sh            (assumes ./debs, ./third-party, ./libs are present)
#
# See README.md for where every source file comes from.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
BT=$here
PROJ=$(cd "$BT/../../.." && pwd)              # .../tempotec-v3
TC=$PROJ/toolchain/rockbox-toolchain
CROSS=$TC/bin/mipsel-rockbox-linux-gnu

SYS=$BT/sysroot
BUILD=$BT/build
OUT=$BT/out
SHIM_SRC=$PROJ/src/sonix-blaze/tools/glibc216_shim.c

LIBS="-lrt -lz -lffi -lm -lgmodule-2.0"

log() { printf '\n==> %s\n' "$*"; }

# ---------------------------------------------------------------------------
# 1. pseudo-sysroot: headers + .pc from Debian mipsel debs, .so from the device
# ---------------------------------------------------------------------------

deb_extract() { # $1 = .deb  $2 = destination dir
	local tmp data
	tmp=$(mktemp -d)
	( cd "$tmp" && ar x "$1" )
	data=$(ls "$tmp"/data.tar.* | head -1)
	tar -C "$2" -xf "$data"
	rm -rf "$tmp"
}

mk_sysroot() {
	log "building pseudo-sysroot in $SYS"
	rm -rf "$SYS"
	mkdir -p "$SYS/usr/include" "$SYS/usr/lib" "$SYS/usr/lib/pkgconfig" "$SYS/usr/bin"

	# --- headers + .pc from the dev debs -----------------------------------
	for deb in \
		libglib2.0-dev_2.42.1-1+b1_mipsel.deb \
		libdbus-1-dev_1.10.32-0+deb9u1_mipsel.deb \
		libfdk-aac-dev_2.0.1-1_mipsel.deb \
		libopenaptx-dev_0.2.0-5_mipsel.deb \
		libldacbt-enc-dev_2.0.2.3+git20200429+ed310a0-4_mipsel.deb \
		libldacbt-abr-dev_2.0.2.3+git20200429+ed310a0-4_mipsel.deb \
		; do
		[ -f "$BT/debs/$deb" ] || { echo "missing deb: $deb" >&2; exit 1; }
		deb_extract "$BT/debs/$deb" "$SYS"
	done

	# Debian multiarch puts the .pc files, glibconfig.h and dbus-arch-deps.h
	# under usr/lib/<triplet>/; the .pc files point at usr/lib/, so flatten.
	if [ -d "$SYS/usr/lib/mipsel-linux-gnu" ]; then
		cp -a "$SYS/usr/lib/mipsel-linux-gnu/." "$SYS/usr/lib/"
		rm -rf "$SYS/usr/lib/mipsel-linux-gnu"
	fi

	# Debian ships its own (host-targeted) .a/.so in the debs; drop them and
	# use the device's runtime libraries instead.
	rm -f "$SYS"/usr/lib/lib*.a "$SYS"/usr/lib/lib*.so "$SYS"/usr/lib/lib*.so.*

	# --- ALSA (1.0.26) from the rockbox toolchain sysroot ------------------
	mkdir -p "$SYS/usr/include/alsa"
	cp -a "$TC/mipsel-rockbox-linux-gnu/sysroot/usr/include/alsa/." "$SYS/usr/include/alsa/"
	cp -a "$TC/mipsel-rockbox-linux-gnu/sysroot/usr/lib/libasound.so"* "$SYS/usr/lib/"
	cp "$TC/mipsel-rockbox-linux-gnu/sysroot/usr/lib/pkgconfig/alsa.pc" "$SYS/usr/lib/pkgconfig/"

	# --- BlueZ public headers (from the bluez tarball) + bluez.pc ----------
	local bz
	bz=$(mktemp -d)
	tar -C "$bz" -xf "$BT/third-party/bluez-5.66.tar.xz"
	mkdir -p "$SYS/usr/include/bluetooth"
	cp "$bz"/bluez-5.66/lib/*.h "$SYS/usr/include/bluetooth/"
	rm -rf "$bz"

	# --- runtime .so the linker must see (from ./libs) ---------------------
	cp "$BT"/libs/libglib-2.0.so.0.4600.1 "$SYS/usr/lib/"
	cp "$BT"/libs/libgio-2.0.so.0.4600.1 "$SYS/usr/lib/"
	cp "$BT"/libs/libgobject-2.0.so.0.4600.1 "$SYS/usr/lib/"
	cp "$BT"/libs/libgmodule-2.0.so.0.4600.1 "$SYS/usr/lib/"
	cp "$BT"/libs/libdbus-1.so.3.14.4 "$SYS/usr/lib/"
	cp "$BT"/libs/libfdk-aac.so.2 "$SYS/usr/lib/"
	cp "$BT"/libs/libopenaptx.so.0 "$SYS/usr/lib/"
	cp "$BT"/libs/libldacBT.so "$SYS/usr/lib/"
	# zlib and libffi are pulled in by glib's dependencies (and by the
	# make LIBS override); they are on the device too.
	cp "$BT"/libs/libz.so.1.2.8 "$SYS/usr/lib/"
	cp "$BT"/libs/libffi.so.6.0.2 "$SYS/usr/lib/"
	ln -sf libz.so.1.2.8 "$SYS/usr/lib/libz.so"
	ln -sf libffi.so.6.0.2 "$SYS/usr/lib/libffi.so"
	ln -sf libglib-2.0.so.0.4600.1 "$SYS/usr/lib/libglib-2.0.so"
	ln -sf libgio-2.0.so.0.4600.1 "$SYS/usr/lib/libgio-2.0.so"
	ln -sf libgobject-2.0.so.0.4600.1 "$SYS/usr/lib/libgobject-2.0.so"
	ln -sf libgmodule-2.0.so.0.4600.1 "$SYS/usr/lib/libgmodule-2.0.so"
	ln -sf libdbus-1.so.3.14.4 "$SYS/usr/lib/libdbus-1.so"
	ln -sf libfdk-aac.so.2 "$SYS/usr/lib/libfdk-aac.so"
	ln -sf libopenaptx.so.0 "$SYS/usr/lib/libopenaptx.so"
	# libbluetooth.so comes from the device (our BlueZ 5.66 build) ----------
	if [ -f "$BT/libs/libbluetooth.so.3" ]; then
		cp "$BT"/libs/libbluetooth.so.3 "$SYS/usr/lib/"
		ln -sf libbluetooth.so.3 "$SYS/usr/lib/libbluetooth.so"
	fi

	# --- normalise the Debian .pc files -----------------------------------
	# ${prefix}/lib/mipsel-linux-gnu -> ${prefix}/lib, absolute /usr/lib -> lib
	local pc
	for pc in "$SYS"/usr/lib/pkgconfig/*.pc; do
		sed -i \
			-e 's#/usr/lib/mipsel-linux-gnu#/usr/lib#g' \
			-e 's#lib/mipsel-linux-gnu#lib#g' \
			-e 's#^libdir=/usr/lib#libdir=${prefix}/lib#' \
			"$pc"
	done

	# Hand-written .pc files live in ./pc (so they are in the repository, not
	# only in this generated sysroot): bluez.pc, the three ldacBT-*.pc (all
	# pointing at the device's combined libldacBT.so) and the empty
	# libpcre/libselinux/libmount stubs that glib/gio's Requires.private needs.
	cp "$BT"/pc/*.pc "$SYS/usr/lib/pkgconfig/"

	# The decoder prototypes are absent from the AOSP-derived ldacBT.h that
	# the Debian package ships (upstream removed the decoder). bluez-alsa
	# 4.0.0 needs them; they exist in the device's libldacBT.so.
	local hdr="$SYS/usr/include/ldac/ldacBT.h"
	if ! grep -q 'ldacBT_init_handle_decode' "$hdr"; then
		cat >> "$hdr" <<'EOF'

/* --- LDAC decoder API (present in the device's combined libldacBT.so;
 *     the upstream AOSP header carries the encoder only) --- */
LDACBT_API int ldacBT_init_handle_decode( HANDLE_LDAC_BT hLdacBt, int cm, int sf,
                                          int nbands, int bitrate, int mtu );
LDACBT_API int ldacBT_decode( HANDLE_LDAC_BT hLdacBt, void *p_l2cap_packet,
                              void *p_pcm, LDACBT_SMPL_FMT_T fmt, int payload_len,
                              int *p_used, int *p_decoded );
EOF
	fi
}

# ---------------------------------------------------------------------------
# 2. sbc 2.1 -- static libsbc.a + sbc.pc (bluez >= 5.5 moved SBC out of tree)
# ---------------------------------------------------------------------------

build_sbc() {
	log "building sbc 2.1 (static)"
	local src=$BUILD/sbc-2.1
	rm -rf "$src"
	mkdir -p "$BUILD"
	tar -C "$BUILD" -xf "$BT/third-party/sbc-2.1.tar.xz"
	(
		cd "$src"
		./configure --host=mipsel-rockbox-linux-gnu --prefix=/usr \
			--disable-shared --enable-static --disable-tester --disable-tools \
			CC="$CROSS-gcc" AR="$CROSS-ar" RANLIB="$CROSS-ranlib" \
			CFLAGS="-O2 -march=mips32r2" >/dev/null
		make -j"$(nproc)" >/dev/null
		make install DESTDIR="$SYS" >/dev/null
	)
	# sbc.pc installs with prefix=/usr; that is what we want.
}

# ---------------------------------------------------------------------------
# 3. bluez-alsa 4.0.0 + patch
# ---------------------------------------------------------------------------

build_bluealsa() {
	log "building bluealsa 4.0.0 (+ --io-rt-priority)"
	local src=$BUILD/bluez-alsa-4.0.0
	rm -rf "$src"
	cp -a "$BT/src/bluez-alsa-4.0.0" "$src"
	( cd "$src" && patch -p1 < "$BT/patch/0001-io-rt-priority.patch" )

	export PATH="$TC/bin:$PATH"
	export PKG_CONFIG_SYSROOT_DIR="$SYS"
	export PKG_CONFIG_LIBDIR="$SYS/usr/lib/pkgconfig"
	export PKG_CONFIG_PATH=""

	(
		cd "$src"
		# The GitHub tag snapshot ships configure.ac only; generate configure.
		autoreconf -fi > "$BUILD/autoreconf.log" 2>&1 || { tail -40 "$BUILD/autoreconf.log"; exit 1; }
		./configure --host=mipsel-rockbox-linux-gnu --prefix=/usr \
			CC="$CROSS-gcc" AR="$CROSS-ar" RANLIB="$CROSS-ranlib" STRIP="$CROSS-strip" \
			CFLAGS="-O2 -march=mips32r2" \
			LDFLAGS="-Wl,--allow-shlib-undefined" \
			--enable-aac --enable-aptx --enable-aptx-hd --enable-ldac --with-libopenaptx \
			--disable-aplay --disable-systemd --disable-upower \
			> "$BUILD/configure.log" 2>&1 || { tail -40 "$BUILD/configure.log"; exit 1; }
		make -j"$(nproc)" LIBS="$LIBS" > "$BUILD/make.log" 2>&1 || { tail -60 "$BUILD/make.log"; exit 1; }

		# --- relink the ALSA plugin with the glibc-2.16 shim ---------------
		# The plugin uses clock_gettime(); defining it locally (the shim)
		# removes the versioned GLIBC_2.17 reference entirely.
		$CROSS-gcc -O2 -march=mips32r2 -fPIC -c "$SHIM_SRC" -o "$BUILD/glibc216_shim.o"
		$CROSS-gcc -shared -Wl,-soname,libasound_module_pcm_bluealsa.so \
			-o src/asound/.libs/libasound_module_pcm_bluealsa.so \
			src/asound/.libs/bluealsa-pcm.o \
			src/shared/.libs/a2dp-codecs.o \
			src/shared/.libs/dbus-client.o \
			src/shared/.libs/hex.o \
			src/shared/.libs/log.o \
			src/shared/.libs/rt.o \
			"$BUILD/glibc216_shim.o" \
			-L"$SYS/usr/lib" -lasound -ldbus-1 $LIBS \
			-Wl,--allow-shlib-undefined
	)
}

# ---------------------------------------------------------------------------
# 4. collect the artefacts
# ---------------------------------------------------------------------------

collect() {
	log "collecting artefacts into $OUT"
	mkdir -p "$OUT"
	cp "$BUILD/bluez-alsa-4.0.0/src/bluealsa" "$OUT/bluealsa"
	cp "$BUILD/bluez-alsa-4.0.0/src/asound/.libs/libasound_module_pcm_bluealsa.so" \
		"$OUT/libasound_module_pcm_bluealsa.so"
	ls -la "$OUT"
}

mk_sysroot
build_sbc
build_bluealsa
collect

log "done. artefacts in $OUT"
