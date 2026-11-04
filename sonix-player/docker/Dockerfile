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
RUN apt-get update && apt-get install -y --no-install-recommends \
		build-essential autoconf automake libtool libtool-bin texinfo pkg-config \
		libgmp-dev libmpfr-dev libmpc-dev gawk bison gtk-doc-tools \
		wget git xz-utils bzip2 patch file python3 p7zip-full ca-certificates \
	&& rm -rf /var/lib/apt/lists/*

# Nothing is built into the image. The tree is mounted at its own absolute path
# and everything the build produces -- rockbox-toolchain/, freetype-target/,
# audio-target/, build_target/ -- stays in it, so the first run pays for the
# toolchain and the libraries and the runs after it only compile what changed.
# docker/build.sh does the mounting.
WORKDIR /work
