#!/usr/bin/env python3
"""Wrap bare pixel literals in SC() in the layout calls of src/gui.

The interface is written against a 480x720 panel and SC() (src/gui/shell/scale.h)
turns those numbers into the panel's own at compile time. Upstream code arrives
with the 480x720 numbers in it, so every merge needs this pass over the pages.

    python3 tools/scale_gui.py --check src/gui/audio/metronomepage.c   # report only
    python3 tools/scale_gui.py --write src/gui/audio/metronomepage.c

It is not the whole of the scaling work: the `#define`d sizes at the top of a
page have to be wrapped by hand (`#define PILL_H SC(56)`), and a number that is
not a size -- a count, a duration in milliseconds, a percentage -- must not be.
Nor must a number that is already in another space: topbar.c's BATTERY_CAVITY_*
are 24x24 SVG coordinates that draw_battery() scales itself, and wrapping them
shrank the level bar.
Run it from sonix-player/ after merging upstream, then look at the pages in the
simulator (make host) before believing it.

Only the arguments that are pixels are touched, and only when they are a bare
integer literal: a percentage (lv_pct), LV_SIZE_CONTENT, a variable, a call or
an existing SC() is left alone. Zero is left alone too -- SC(0) is noise.
"""

import argparse
import re
import sys

# function -> 1-based indexes of the arguments that are pixels (counted from the
# opening parenthesis of the call, splitting on top-level commas)
PIXEL_ARGS = {
    "lv_obj_set_size": (2, 3),
    "lv_obj_set_width": (2,),
    "lv_obj_set_height": (2,),
    "lv_obj_set_pos": (2, 3),
    "lv_obj_set_x": (2,),
    "lv_obj_set_y": (2,),
    "lv_obj_align": (3, 4),
    "lv_obj_align_to": (3, 4),
    "lv_obj_set_content_width": (2,),
    "lv_obj_set_content_height": (2,),
    "lv_obj_set_ext_click_area": (2,),
    "lv_obj_set_style_pad_all": (2,),
    "lv_obj_set_style_pad_hor": (2,),
    "lv_obj_set_style_pad_ver": (2,),
    "lv_obj_set_style_pad_row": (2,),
    "lv_obj_set_style_pad_column": (2,),
    "lv_obj_set_style_pad_top": (2,),
    "lv_obj_set_style_pad_bottom": (2,),
    "lv_obj_set_style_pad_left": (2,),
    "lv_obj_set_style_pad_right": (2,),
    "lv_obj_set_style_radius": (2,),
    "lv_obj_set_style_border_width": (2,),
    "lv_obj_set_style_outline_width": (2,),
    "lv_obj_set_style_outline_pad": (2,),
    "lv_obj_set_style_shadow_width": (2,),
    "lv_obj_set_style_shadow_ofs_x": (2,),
    "lv_obj_set_style_shadow_ofs_y": (2,),
    "lv_obj_set_style_shadow_spread": (2,),
    "lv_obj_set_style_line_width": (2,),
    "lv_obj_set_style_text_letter_space": (2,),
    "lv_obj_set_style_text_line_space": (2,),
    "lv_obj_set_style_translate_x": (2,),
    "lv_obj_set_style_translate_y": (2,),
    "lv_obj_set_style_margin_top": (2,),
    "lv_obj_set_style_margin_bottom": (2,),
    "lv_obj_set_style_margin_left": (2,),
    "lv_obj_set_style_margin_right": (2,),
    "lv_obj_set_style_min_width": (2,),
    "lv_obj_set_style_min_height": (2,),
    "lv_obj_set_style_max_width": (2,),
    "lv_obj_set_style_max_height": (2,),
    "lv_obj_set_style_transform_width": (2,),
    "lv_obj_set_style_transform_height": (2,),
    # Not LVGL: src/gui/shell/keyboard.c, the tray's width and height. It is the
    # one place the interface hands a pixel size to its own code, and three pages
    # passed 316 unscaled because the regex below knew nothing about it -- the
    # keyboard came out a third taller than every other page's.
    "keyboard_create": (2, 3),
}

CALL_RE = re.compile(r"\b(" + "|".join(PIXEL_ARGS) + r")\s*\(")
INT_RE = re.compile(r"^-?\d+$")

# Bare numbers that are not pixels but LVGL's own constants.
KEEP = {"0", "1", "-1"}


def split_args(text):
    """Split a call's argument text on top-level commas, keeping the pieces."""
    out, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append((start, i))
            start = i + 1
    out.append((start, len(text)))
    return out


def process(src):
    """Return (new_source, [(line_no, call, old, new), ...])."""
    changes = []
    out = []
    i = 0
    line_no = 1
    while True:
        m = CALL_RE.search(src, i)
        if not m:
            out.append(src[i:])
            break
        out.append(src[i : m.end()])
        line_no += src[i : m.end()].count("\n")
        # find the matching close paren
        depth, j = 1, m.end()
        while j < len(src) and depth:
            if src[j] == "(":
                depth += 1
            elif src[j] == ")":
                depth -= 1
            j += 1
        args = src[m.end() : j - 1]
        spans = split_args(args)
        spans.reverse()  # right to left, so earlier offsets stay valid
        new_args = args
        for idx in sorted(PIXEL_ARGS[m.group(1)], reverse=True):
            if idx > len(spans):
                continue
            a, b = spans[len(spans) - idx]
            piece = new_args[a:b]
            stripped = piece.strip()
            if not INT_RE.match(stripped) or stripped in KEEP:
                continue
            head = piece[: len(piece) - len(piece.lstrip())]
            tail = piece[len(piece.rstrip()) :]
            new_piece = f"{head}SC({stripped}){tail}"
            new_args = new_args[:a] + new_piece + new_args[b:]
            changes.append((line_no, m.group(1), stripped, f"SC({stripped})"))
        out.append(new_args + ")")
        i = j
    return "".join(out), changes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    status = 0
    for path in args.files:
        src = open(path, encoding="utf-8", errors="surrogateescape").read()
        new, changes = process(src)
        if not changes:
            continue
        if args.check:
            for line, call, old, newv in changes:
                print(f"{path}:{line}: {call} {old} -> {newv}")
            status = 1
            continue
        if args.write:
            open(path, "w", encoding="utf-8", errors="surrogateescape").write(new)
            print(f"{path}: {len(changes)} literals scaled")
    sys.exit(status)


if __name__ == "__main__":
    main()
