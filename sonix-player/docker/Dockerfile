# The environment `make target` runs in.
#
# The player is a MIPS binary, but nothing about building it is MIPS: the
# toolchain is the Rockbox one, built here from binutils/gcc/glibc sources by
# sonix-player/rockboxdev/rockboxdev.sh, and the libraries it links (FreeType,
# libogg/opus/opusfile, libwavpack) are cross-built from their own tarballs.
# What all of that does need is a native compiler, makeinfo, autoconf, automake,
# libtool, wget, xz and bzip2 on the machine running it -- a list long enough
# that installing it for one build is the wrong trade. This image carries the
# list, and the build runs in it:
#
#   docker build -t tempotec-player-build sonix-player/docker
#   sonix-player/docker/build.sh            # the player and the launcher
#   sonix-player/docker/build.sh host       # the SDL simulator instead
#
# The base is pinned by digest, and it is the same Ubuntu 24.04 the packer's
# image (sonix-packer/docker) uses, so an image built today is the image a clone
# builds later.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

ENV DEBIAN_FRONTEND=noninteractive

# What each one is for, so that none of them looks like an accident:
#
#   build-essential   gcc, g++ and make: the toolchain's own build (binutils,
#                     gcc 9.5, glibc 2.27) and the configure probes of every
#                     library that follows
#   autoconf, automake, libtool, libtool-bin
#                     rockboxdev.sh checks for them by name; binutils and gcc
#                     are built from release tarballs but with their own
#                     autotools, and FreeType's and the audio libraries'
#                     configure scripts are the same generation. libtool-bin is
#                     a package of its own on Debian and Ubuntu: `libtool`
#                     carries the m4 macros and libtoolize, the script itself
#                     comes with libtool-bin, and rockboxdev.sh looks for the
#                     script by name
#   texinfo           makeinfo, also checked by name
#   pkg-config        configure looks for it; the libraries here are all static
#                     and configured by hand, so nothing is resolved through it
#   libgmp-dev, libmpfr-dev, libmpc-dev
#                     the three libraries gcc is built against; rockboxdev.sh
#                     checks for them by linking a test program with -lmpc,
#                     -lgmp and -lmpfr, so it needs the development packages
#                     and not only the runtime ones gcc itself pulls in
#   gawk, bison       glibc's configure refuses to go on without them, by name
#   gtk-doc-tools     glib 2.46 is rebuilt with autoreconf (its m4 macros are
#                     from 2015), and the aclocal run there calls gtkdocize
#   wget, xz-utils, bzip2
#                     the toolchain, FreeType and the audio tarballs
#   patch             rockboxdev.sh applies rockboxdev/toolchain-patches/
#   git               the Makefile clones LVGL 9.6 when sonix-player/lvgl is
#                     absent
#   python3           generate_compile_commands.py, which `make target` runs
#   file, p7zip-full  looking at a .upt and at the binaries: `file sonix_player`,
#                     `7z l v3_analog.upt`
#   ca-certificates   https for all of the above
#
# Rust is not in this list: the distribution's rustc 1.75 cannot build the
# Spotify engine's MIPS half, and the reason is the whole section below it.
RUN apt-get update && apt-get install -y --no-install-recommends \
		build-essential autoconf automake libtool libtool-bin texinfo pkg-config \
		libgmp-dev libmpfr-dev libmpc-dev gawk bison gtk-doc-tools \
		wget git xz-utils bzip2 patch file python3 p7zip-full ca-certificates \
	&& rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# Rust, for the MIPS half of the Spotify addon
# (addons/spotify/engine, proven by addons/spotify/engine-mips)
# ---------------------------------------------------------------------------
#
# `make addons` builds the addon's Rust half with whatever cargo is on the
# machine, and for a long time that was the distribution's rustc 1.75: the
# crate has no dependencies, so a plain `cargo build` needed no index, no
# vendor directory and no toolchain download. What 1.75 cannot do is build for
# the device. `mipsel-unknown-linux-musl` is a Tier 3 target, which means rustup
# has no prebuilt standard library for it, which means the only way to a binary
# is to compile std from source for the target -- `-Z build-std`, nightly only,
# and it reads the source from the `rust-src` component. That is the entire
# reason rustup is here.
#
# The nightly is pinned by date rather than by channel: `nightly` moves every
# night, and an image built in a year should produce the binary this image
# produces. nightly-2026-10-01 is rustc 1.101.0-nightly (21b707e3f 2026-09-30)
# and does publish `rust-src`; moving the pin means checking that it still does,
# and re-running the engine-mips proof (its README has the command).
#
# rustup-init itself is pinned and fetched by wget, which the image already has,
# rather than through sh.rustup.rs: one fewer network hop and one fewer script
# that can change under a pinned base image.
#
# RUSTUP_HOME and CARGO_HOME are moved out of $HOME on purpose.
# docker/build.sh runs the build as the calling user with HOME=/tmp, so a
# toolchain installed into /root/.cargo would be invisible to it. They are made
# world-writable for the same reason: that user has to be able to write the
# registry cache, the download cache and rustup's own settings.
ENV RUSTUP_HOME=/opt/rustup \
	CARGO_HOME=/opt/cargo
RUN wget -q -O /tmp/rustup-init \
		https://static.rust-lang.org/rustup/archive/1.28.2/x86_64-unknown-linux-gnu/rustup-init \
	&& chmod +x /tmp/rustup-init \
	&& /tmp/rustup-init -y --no-modify-path --profile minimal \
		--default-toolchain nightly-2026-10-01 \
		--component rust-src \
	&& rm -f /tmp/rustup-init \
	&& chmod -R a+rwX "$RUSTUP_HOME" "$CARGO_HOME"
ENV PATH=/opt/cargo/bin:$PATH

# A C cross-compiler for mipsel-linux-musl, and the reason it is here rather
# than "rustc can do MIPS on its own": rustc's LLVM does emit MIPS32r2, but the
# static link still needs a C library and the four crt objects, and for a
# distributed target those come from rustc's own `self-contained` directory --
# a directory that does not exist for a Tier 3 target that was just compiled
# with `-Z build-std`. musl.cc publishes a complete mipsel-linux-musl gcc
# (musl 1.2, built for i686 hosts and runnable on x86-64), unpacked where its
# tarball expects to be, so `-C linker=mipsel-linux-musl-gcc` links against its
# `libc.a` and `crt1.o`. It is also the `cc` for any C dependency the engine
# grows later, and the toolchain `cc`-using build scripts look for.
#
# The last line of that RUN is the one thing that is not the tarball's own
# layout. rustc adds `-lunwind` to every musl link unconditionally, on the
# assumption that the target is distributed and ships LLVM's libunwind in its
# `self-contained` directory -- which a Tier 3 target built here does not. The
# name is all that is missing: libgcc_eh.a is the same Itanium unwinder ABI
# (`_Unwind_RaiseException`, `_Unwind_Resume`, ...) compiled for the same
# target, and it is in the tarball already. So it is put where `-lunwind` looks,
# under the name rustc asks for. Nothing unwinds in the engine (panic = "abort",
# no backtrace printing), but the linker is not told that and the symbol must
# resolve.
#
# The tarball is not checksummed: musl.cc publishes no manifest, and the file
# has not changed since 2021 (Last-Modified: Tue, 23 Nov 2021). If that ever
# matters, the tarball is in the image layer and `sha256sum` on it is the pin.
ENV PATH=/opt/mipsel-linux-musl-cross/bin:$PATH
RUN wget -q -O /tmp/musl-cross.tgz https://musl.cc/mipsel-linux-musl-cross.tgz \
	&& tar -xzf /tmp/musl-cross.tgz -C /opt \
	&& rm -f /tmp/musl-cross.tgz \
	&& cp /opt/mipsel-linux-musl-cross/lib/gcc/mipsel-linux-musl/*/libgcc_eh.a \
		/opt/mipsel-linux-musl-cross/mipsel-linux-musl/lib/libunwind.a \
	&& mipsel-linux-musl-gcc --version | head -1

# Nothing is built into the image. The tree is mounted at its own absolute path
# and everything the build produces -- rockbox-toolchain/, freetype-target/,
# audio-target/, build_target/ -- stays in it, so the first run pays for the
# toolchain and the libraries and the runs after it only compile what changed.
# docker/build.sh does the mounting.
WORKDIR /work
