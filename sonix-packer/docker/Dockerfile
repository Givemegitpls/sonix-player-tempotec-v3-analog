# The image the firmware build runs its tools in.
#
# `v3_analog_packer.sh` unpacks the stock `.upt`, extracts the UBIFS from it,
# repacks the tree with `mkfs.ubifs` and builds the ISO with `genisoimage` --
# and none of those exist on a normal host. The packer runs them in this image
# instead (see `fw()` in the script), mounted at `/packer`.
#
#     docker build -t tempotec-fw-tools sonix-packer/docker
#
# The host itself needs only docker: the script refuses to start without the
# image, and says so by name.
#
# The base is pinned by digest and ubi_reader by version, so that an image built
# today is the same image a clone builds in a year. The rest of the list is for
# the neighbouring work: a kernel-only image needs `genisoimage` too (see
# `docs/KERNEL.md` and `patch/v3-panel-blank/`), and the analysis tools under
# `patch/` and `tools/` need capstone, pyelftools and lzo.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        mtd-utils \
        genisoimage \
        p7zip-full \
        python3 \
        python3-pip \
        python3-lzo \
        python3-pyelftools \
        python3-capstone \
        cpio \
        squashfs-tools \
        binutils \
        radare2 \
        patch \
        file \
        less \
        vim-common \
        ca-certificates \
    && pip3 install --break-system-packages --no-cache-dir ubi_reader==0.8.16 \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /packer
