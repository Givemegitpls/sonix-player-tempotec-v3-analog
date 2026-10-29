#!/usr/bin/env python3
"""Decode the V3 boot-blink timeline out of a video recording.

Takes any video the owner recorded of the player screen (phone camera is the
reliable source; the C270 webcam eats short flashes with its auto exposure).
Prints every dark run (backlight off) and lit run with wall-clock times,
so the agent can read the boot-marker protocol out of it.

    ./blink_decode.py VID.mp4 [--fps F] [--thr N] [--frame F]

Without --thr the threshold is auto-picked: the file's luminance histogram is
bimodal (dark runs vs lit runs), and the midpoint between the two clusters is
used. --frame N additionally prints the mean of a single frame (debugging).
"""
from __future__ import annotations

import argparse
import subprocess
import sys

import numpy as np

FW, FH = 96, 54  # analysis resolution; blinks are whole-frame events


def frame_means(path: str, scale: str = f"scale={FW}:{FH}") -> tuple[np.ndarray, float]:
    """Decode the video as tiny grayscale frames, return per-frame mean."""
    proc = subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
         "-vf", scale, "-f", "rawvideo", "-pix_fmt", "gray", "-"],
        stdout=subprocess.PIPE,
    )
    assert proc.stdout is not None
    fs = FW * FH
    means: list[float] = []
    chunk = proc.stdout.read(fs)
    while len(chunk) == fs:
        means.append(float(np.frombuffer(chunk, dtype=np.uint8).mean()))
        chunk = proc.stdout.read(fs)
    proc.wait()
    return np.asarray(means), proc.returncode


def auto_threshold(m: np.ndarray) -> float:
    """Midpoint of the gap between the dark cluster and the lit cluster.

    Bimodal histogram: find the largest empty stretch between two populated
    bins and take its middle.
    """
    hist, edges = np.histogram(m, bins=64, range=(0, 256))
    nonzero = np.where(hist > 0)[0]
    if nonzero.size == 0:
        return 15.0
    lo, hi = nonzero[0], nonzero[-1]
    if hi - lo < 8:  # single cluster; fall back to a low absolute cut
        return float(max(4.0, edges[lo] + 1))
    # largest empty gap that actually separates two clusters:
    # both sides must hold >= 20% of the frames
    gaps = []
    run_start = None
    total = m.size
    for k in range(lo, hi + 1):
        if hist[k] == 0:
            if run_start is None:
                run_start = k
        else:
            if run_start is not None:
                below = hist[:k].sum() / total
                if 0.2 <= below <= 0.8:
                    gaps.append((k - run_start, run_start, k))
                run_start = None
    if not gaps:  # no gap between two real clusters; absolute low cut
        return float(max(8.0, np.percentile(m, 40) * 0.5))
    width, a, b = max(gaps)
    return float((edges[a] + edges[b]) / 2)


def runs_of(mask: np.ndarray) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    i, n = 0, mask.size
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]:
                j += 1
            out.append((i, j - 1))
            i = j
        else:
            i += 1
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("video")
    ap.add_argument("--fps", type=float, default=0.0,
                    help="frames per second (default: from ffprobe)")
    ap.add_argument("--thr", type=float, default=0.0,
                    help="dark threshold on the frame mean (0 = auto)")
    ap.add_argument("--frame", type=int, default=-1,
                    help="print the mean of this frame number and exit")
    args = ap.parse_args()

    if not args.fps:
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=avg_frame_rate", "-of", "default=nw=1:nk=1",
             args.video],
            capture_output=True, text=True)
        num, _, den = (probe.stdout.strip() or "30/1").partition("/")
        args.fps = float(num) / float(den or 1)

    m, rc = frame_means(args.video)
    if m.size == 0:
        print("no frames decoded", file=sys.stderr)
        return 1
    n = m.size
    dur = n / args.fps
    print(f"frames={n} fps={args.fps:g} duration={dur:.1f}s")
    print(f"mean: min={m.min():.1f} max={m.max():.1f} median={np.median(m):.1f}")
    if args.frame >= 0:
        if args.frame < n:
            print(f"frame {args.frame}: mean={m[args.frame]:.1f}")
        else:
            print(f"frame {args.frame} out of range (n={n})")
            return 1

    thr = args.thr if args.thr > 0 else auto_threshold(m)
    dark = m < thr
    print(f"threshold={thr:.1f}  dark frames: {dark.sum()} of {n} ({dark.mean() * 100:.1f}%)")

    dark_runs = runs_of(dark)
    lit_runs = runs_of(~dark)
    print(f"\n== dark runs (backlight off) ==")
    for k, (a, b) in enumerate(dark_runs):
        gap = (a / args.fps) - (lit_runs[k - 1][1] / args.fps) if k else float("nan")
        print(f"frames {a:5d}-{b:5d}  t {a / args.fps:7.2f}s - {b / args.fps:7.2f}s"
              f"  ({(b - a + 1) / args.fps:6.2f}s, {b - a + 1} frames)"
              + (f"  gap before {gap:5.2f}s" if k else "  (start)"))
    print("\n== lit runs (backlight on) ==")
    for a, b in lit_runs:
        print(f"frames {a:5d}-{b:5d}  t {a / args.fps:7.2f}s - {b / args.fps:7.2f}s"
              f"  ({(b - a + 1) / args.fps:6.2f}s, {b - a + 1} frames)")
    return rc if rc else 0


if __name__ == "__main__":
    sys.exit(main())
