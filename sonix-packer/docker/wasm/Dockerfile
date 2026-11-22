# The image `mkfs.ubifs` is compiled in, for the in-browser firmware page.
#
# The page rebuilds the vendor's image locally: it reads the stock `.upt`, edits
# the rootfs tree in memory and has to produce a UBIFS image the device will
# mount. That last step is `mkfs.ubifs`, whose write path (index B-tree, LEB
# allocation, LPT, space accounting, orphan area) is nothing anyone should
# reimplement in JavaScript. It is compiled once, to WebAssembly, here -- and the
# same module exports the LZO decompressor the page reads the stock image back
# with, so the reader and the writer compress through one implementation.
#
#     docker build -t tempotec-wasm-tools sonix-packer/docker/wasm
#     sonix-packer/tools/wasm/build.sh          # builds the image if missing
#
# The versions below are pinned to the same mtd-utils the firmware tools image
# carries (Ubuntu 24.04's 2.2.0), so a browser build and a packer build of the
# same tree come out of the same code.
#
# What is in here is build tooling only: emscripten, and what downloads and
# unpacks the two source tarballs. No vendor firmware, no player binaries.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        emscripten \
        make \
        ca-certificates \
        curl \
        xz-utils \
        file \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /build
