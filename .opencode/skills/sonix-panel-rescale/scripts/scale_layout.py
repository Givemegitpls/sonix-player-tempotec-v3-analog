#!/usr/bin/env python3
"""Wrap absolute layout numbers in SC() across the interface.

The interface is authored at 480x720; the TempoTec V3 Analog panel is 320x480,
exactly 2/3. Every distance written down as a literal or as a named constant has
to go through SC() (src/gui/shell/scale.h) so it comes out two thirds smaller on
that panel and unchanged on the others.

Mechanical part done here:
  * a bare integer literal passed to a known coordinate argument becomes SC(n);
  * a numeric #define whose name is used as a whole coordinate argument is
    wrapped too.

Identifiers used inside coordinate expressions (counts, ratios, offsets built
from other values) are reported for review, not touched.
"""

import os
import re
import sys

# The player tree to rewrite. Default: this project's checkout, worked out from
# this file's own path (.opencode/skills/<name>/scripts/ -> project root).
ROOT = (
    sys.argv[1]
    if len(sys.argv) > 1
    else os.path.join(
        os.path.dirname(
            os.path.dirname(
                os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
            )
        ),
        "sonix-player",
    )
)

COORD = {
    "lv_obj_set_size": [1, 2],
    "lv_obj_set_width": [1],
    "lv_obj_set_height": [1],
    "lv_obj_set_x": [1],
    "lv_obj_set_y": [1],
    "lv_obj_set_pos": [1, 2],
    "lv_obj_align": [2, 3],
    "lv_obj_align_to": [3, 4],
    "lv_obj_set_style_size": [1, 2],
    "lv_obj_set_style_pad_all": [1],
    "lv_obj_set_style_pad_hor": [1],
    "lv_obj_set_style_pad_ver": [1],
    "lv_obj_set_style_pad_row": [1],
    "lv_obj_set_style_pad_column": [1],
    "lv_obj_set_style_pad_top": [1],
    "lv_obj_set_style_pad_bottom": [1],
    "lv_obj_set_style_pad_left": [1],
    "lv_obj_set_style_pad_right": [1],
    "lv_obj_set_style_pad_gap": [1],
    "lv_obj_set_style_radius": [1],
    "lv_obj_set_style_border_width": [1],
    "lv_obj_set_style_shadow_width": [1],
    "lv_obj_set_style_shadow_offset_x": [1],
    "lv_obj_set_style_shadow_offset_y": [1],
    "lv_obj_set_style_margin_top": [1],
    "lv_obj_set_style_margin_bottom": [1],
    "lv_obj_set_style_margin_left": [1],
    "lv_obj_set_style_margin_right": [1],
    "lv_obj_set_style_margin_ver": [1],
    "lv_obj_set_style_margin_hor": [1],
    "lv_obj_set_style_max_width": [1],
    "lv_obj_set_style_max_height": [1],
    "lv_obj_set_style_min_width": [1],
    "lv_obj_set_style_min_height": [1],
    "lv_obj_set_style_translate_x": [1],
    "lv_obj_set_style_translate_y": [1],
    "lv_obj_set_style_text_line_space": [1],
    "lv_obj_set_style_text_letter_space": [1],
    "lv_obj_set_style_arc_width": [1],
    "lv_obj_set_style_outline_width": [1],
    "lv_obj_set_style_line_width": [1],
    "lv_obj_set_ext_click_area": [1],
}
NAMES = sorted(COORD, key=len, reverse=True)
CALL_RE = re.compile(r"\b(" + "|".join(re.escape(n) for n in NAMES) + r")\s*\(")
LITERAL_RE = re.compile(r"^[-+]?[0-9]+$")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
DEFINE_RE = re.compile(r"^#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(\(?[-+]?[0-9]+\)?)\s*$")

# Names that are counts, ratios or durations rather than distances, even when
# they turn up in a coordinate expression.
NON_DISTANCE = re.compile(r"(MAX|MIN|COUNT|NUM|DEN|_ID$|_MS$|_HZ$|RATE|PERCENT|INDEX|DEPTH|SIZE_MAX|_PCT)")


def mask(text):
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            q, j = c, i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == q:
                    j += 1
                    break
                j += 1
            for k in range(i, min(j, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def split_args(s):
    args, depth, cur = [], 0, []
    for c in s:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            args.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    args.append("".join(cur))
    return args


def numeric_defines(text):
    names = {}
    for line in text.splitlines():
        m = DEFINE_RE.match(line.strip())
        if m:
            names[m.group(1)] = m.group(2)
    return names


def process(text, defines):
    masked = mask(text)
    standalone = set()
    inexpr = set()
    wrapped = 0
    out, pos = [], 0
    while True:
        m = CALL_RE.search(masked, pos)
        if not m:
            out.append(text[pos:])
            break
        name = m.group(1)
        open_paren = m.end() - 1
        depth, j = 0, open_paren
        while j < len(masked):
            if masked[j] == "(":
                depth += 1
            elif masked[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if j >= len(masked):
            out.append(text[pos:])
            break
        args = split_args(text[open_paren + 1:j])
        for idx in COORD[name]:
            if idx >= len(args):
                continue
            arg = args[idx]
            stripped = arg.strip()
            if LITERAL_RE.match(stripped) and int(stripped) != 0:
                lead = arg[:len(arg) - len(arg.lstrip())]
                trail = arg[len(arg.rstrip()):]
                args[idx] = f"{lead}SC({stripped}){trail}"
                wrapped += 1
            elif re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", stripped):
                standalone.add(stripped)
            else:
                for ident in IDENT_RE.findall(stripped):
                    if ident in defines:
                        inexpr.add(ident)
        out.append(text[pos:open_paren + 1])
        out.append(",".join(args))
        out.append(")")
        pos = j + 1
    return "".join(out), wrapped, standalone, inexpr


def main():
    files = []
    for base in ("src/gui", "src/main.c"):
        p = os.path.join(ROOT, base)
        if os.path.isfile(p):
            files.append(p)
        else:
            for dirpath, _, names in os.walk(p):
                for nm in names:
                    if nm.endswith((".c", ".h")):
                        files.append(os.path.join(dirpath, nm))

    total = 0
    changed = []
    review = {}
    define_hits = {}

    for path in files:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if "lv_obj_set" not in text and "lv_obj_align" not in text:
            continue
        defines = numeric_defines(text)
        new, wrapped, standalone, inexpr = process(text, defines)
        if new != text:
            with open(path, "w", encoding="utf-8") as f:
                f.write(new)
            changed.append((os.path.relpath(path, ROOT), wrapped))
            total += wrapped
        if standalone:
            define_hits[os.path.relpath(path, ROOT)] = standalone
        if inexpr:
            review[os.path.relpath(path, ROOT)] = inexpr

    # Wrap the numeric #defines used as whole coordinate arguments.
    dwrapped = []
    for rel, names in define_hits.items():
        path = os.path.join(ROOT, rel)
        with open(path, encoding="utf-8") as f:
            lines = f.readlines()
        out, hit = [], False
        for line in lines:
            m = DEFINE_RE.match(line.rstrip("\n").strip())
            if m and m.group(1) in names and not NON_DISTANCE.search(m.group(1)):
                val = m.group(2)
                if not (val.startswith("(") and val.endswith(")")):
                    val = f"({val})"
                indent = line[:len(line) - len(line.lstrip())]
                line = f"{indent}#define {m.group(1)} SC{val}\n"
                hit = True
                dwrapped.append((rel, m.group(1)))
            out.append(line)
        if hit:
            with open(path, "w", encoding="utf-8") as f:
                f.writelines(out)

    print(f"wrapped {total} literals in {len(changed)} files")
    for rel, w in sorted(changed, key=lambda x: -x[1]):
        print(f"  {w:4d}  {rel}")
    print(f"\nwrapped {len(dwrapped)} named constants (used as whole coordinate args)")
    for rel, name in dwrapped:
        print(f"  {rel}: {name}")
    print("\n#defines used INSIDE coordinate expressions (review by hand):")
    for rel in sorted(review):
        for name in sorted(review[rel]):
            print(f"  {rel}: {name}")


if __name__ == "__main__":
    main()
