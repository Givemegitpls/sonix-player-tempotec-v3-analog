#!/usr/bin/env python3
"""Remove the stock libraries nothing in this firmware uses any more.

Runs inside the packer's container after install_tree.sh, on build/SYSTEM --
the rootfs as it will be packed. The stock player (usr/bin/hiby_player) is the
reason these files were in the image at all, and install_tree.sh deletes it: the
decoders it linked (FLAC, LAME, WMA, AAC, MP3, DSD-to-PCM, MAD, SDL) go with it.
The stock glib 2.40 is likewise dead -- this firmware's BlueZ 5 and the player
are built against 2.46.1, which the image carries.

The point is not tidiness. This rootfs is exactly as large as the device's
volume and boots 100% full; replacing a 7.4 MB binary in place on such a tree is
a copy that does not fit, and the ~6 MB this frees is headroom that future
assets need.

Two things are deliberately *not* removed:

  * libcrypto/libssl 1.0.0 -- sbin/wpa_supplicant, sbin/shairport and bin/sshd
    link against them. Only the duplicate copy in usr/lib is dropped, and only
    while the one in /lib is still there (the loader finds /lib first).
  * anything the check below finds a reference to. A library is removed only if
    no remaining ELF file mentions its soname, so a mistake here fails the build
    instead of shipping an image whose Wi-Fi does not come up.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

TREE = Path("build/SYSTEM")

# path under the tree -> why it is dead
DEAD = {
    "usr/lib/libFLAC.so": "stock player's FLAC decoder (hiby_player is deleted)",
    "usr/lib/libFLAC.so.8": "stock player's FLAC decoder (hiby_player is deleted)",
    "usr/lib/libmp3lame.so": "stock player's MP3 encoder/decoder",
    "usr/lib/libmp3lame.so.0": "stock player's MP3 encoder/decoder",
    "usr/lib/libwma.so": "stock player's WMA decoder",
    "usr/lib/libaac.so": "stock player's AAC decoder",
    "usr/lib/libmp3.so": "stock player's MP3 decoder",
    "usr/lib/libdsd2pcm.so": "stock player's DSD-to-PCM converter",
    "usr/lib/libmad.so.0": "stock player's MPEG audio decoder",
    "usr/lib/libSDL-1.2.so.0": "nothing in this firmware uses SDL",
    "usr/lib/libglib-2.0.so.0.4000.0": "stock glib; BlueZ 5 and the player use 2.46.1",
}

# Removed only when the /lib copy is present, because the loader resolves the
# soname there first and these two are byte-for-byte older duplicates.
DUPLICATES = {
    "usr/lib/libcrypto.so.1.0.0": "lib/libcrypto.so.1.0.0",
    "usr/lib/libssl.so.1.0.0": "lib/libssl.so.1.0.0",
}


def remove(rel: str, why: str) -> tuple[int, str]:
    path = TREE / rel
    if not path.is_file() and not path.is_symlink():
        return 0, ""
    size = path.stat().st_size if path.is_file() else len(os.readlink(path))
    path.unlink()
    return size, f"    - {rel} ({size / 1048576:.2f} MB): {why}"


def elf_references(tree: Path, sonames: set[str]) -> list[tuple[str, str]]:
    """Every remaining ELF file that still names one of these sonames."""
    hits: list[tuple[str, str]] = []
    for path in tree.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        try:
            data = path.read_bytes()
        except OSError:
            continue
        if not data.startswith(b"\x7fELF"):
            continue
        for name in sonames:
            if name.encode() in data:
                hits.append((str(path.relative_to(tree)), name))
    return hits


def main() -> int:
    if not (TREE / "usr").is_dir():
        print(f"trim_stock: {TREE} is not the unpacked rootfs", file=sys.stderr)
        return 1

    freed = 0
    lines: list[str] = []
    removed_names: set[str] = set()

    for rel, why in DEAD.items():
        size, line = remove(rel, why)
        if line:
            freed += size
            lines.append(line)
            removed_names.add(Path(rel).name)

    for rel, twin in DUPLICATES.items():
        if not (TREE / twin).is_file():
            lines.append(f"    = {rel} kept: {twin} is not there, and it is the one in use")
            continue
        size, line = remove(rel, f"duplicate of {twin}")
        if line:
            freed += size
            lines.append(line)
            # Deliberately not added to removed_names: the soname still resolves,
            # from the copy the loader finds first. Only a file with no twin at
            # all can leave a hole, and those are kept above.

    for line in lines:
        print(line)

    hits = elf_references(TREE, removed_names)
    if hits:
        print("trim_stock: these files still need a library that was just removed:", file=sys.stderr)
        for path, name in hits:
            print(f"    {path} -> {name}", file=sys.stderr)
        print("trim_stock: add it back to DEAD's exclusions and re-run", file=sys.stderr)
        return 1

    print(f"    trimmed {freed / 1048576:.2f} MB of stock libraries nothing uses")
    return 0


if __name__ == "__main__":
    sys.exit(main())
