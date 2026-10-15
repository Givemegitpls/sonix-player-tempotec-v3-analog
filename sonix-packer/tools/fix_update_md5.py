#!/usr/bin/env python3
"""Recalculate the md5 lines in an UPDATE.TXT after its images were rebuilt.

UPDATE.TXT carries two blocks, kernel={...} and rootfs={...}, each with an
`md5=` line. The updater checks each against the file sitting next to it in the
ISO and refuses an image whose digest does not match, so the lines have to be
rewritten every time the images are packed -- including the kernel block, which
the stock UPDATE.TXT only matches by accident of carrying the stock kernel.

Usage:
    fix_update_md5.py UPDATE.TXT [FILE ...]

Each FILE is matched to a block by its name: SYSTEM.UBI rewrites the rootfs
block, UIMAGE.BIN the kernel block. Files with other names are ignored, so the
old one-argument form (`... UPDATE.TXT SYSTEM.UBI`) still rewrites only rootfs.
"""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

# image file name -> the UPDATE.TXT block it belongs to
BLOCK_OF = {
    "SYSTEM.UBI": "rootfs",
    "UIMAGE.BIN": "kernel",
}


def md5_of(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rewrite_block(text: str, block: str, digest: str) -> str:
    start = text.find(f"{block}={{")
    if start < 0:
        raise SystemExit(f"no {block}={{...}} block in the UPDATE.TXT")
    end = text.find("}", start)
    body, count = re.subn(
        r"(md5=)[0-9a-fA-F]{32}", lambda m: m.group(1) + digest, text[start:end], count=1
    )
    if count != 1:
        raise SystemExit(f"the {block} block has no md5= line")
    return text[:start] + body + text[end:]


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)

    update_txt = Path(sys.argv[1])
    text = update_txt.read_text()

    changed = 0
    for arg in sys.argv[2:]:
        image = Path(arg)
        block = BLOCK_OF.get(image.name)
        if block is None:
            continue
        digest = md5_of(image)
        text = rewrite_block(text, block, digest)
        print(f"    {block} md5 = {digest}")
        changed += 1

    if changed == 0:
        raise SystemExit("nothing to do: no SYSTEM.UBI or UIMAGE.BIN among the arguments")

    update_txt.write_text(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
