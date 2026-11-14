#!/usr/bin/env python3
"""Renders the V field of a run as a PNG image (numpy only, no plotting library).

    render.py --run DIR --out image.png [--scale N]
"""

import argparse
import json
import os
import struct
import zlib

import numpy as np

# A dark-to-bright palette (deep blue, teal, pale yellow), interpolated.
_STOPS = np.array([[0.0, 8, 16, 48], [0.35, 20, 90, 120], [0.7, 120, 200, 150], [1.0, 250, 245, 190]])


def colorize(v):
    lo, hi = float(v.min()), float(v.max())
    t = (v - lo) / (hi - lo) if hi > lo else np.zeros_like(v)
    rgb = np.empty(v.shape + (3,), dtype=np.uint8)
    for c in range(3):
        rgb[..., c] = np.interp(t, _STOPS[:, 0], _STOPS[:, c + 1]).astype(np.uint8)
    return rgb


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", type=int, default=2)
    a = ap.parse_args()
    with open(os.path.join(a.run, "result.json")) as f:
        p = json.load(f)["params"]
    v = np.fromfile(os.path.join(a.run, "V.bin"), dtype=np.float64).reshape(p["height"], p["width"])
    rgb = colorize(v).repeat(a.scale, axis=0).repeat(a.scale, axis=1)
    write_png(a.out, rgb)
    print(f"wrote {a.out} ({rgb.shape[1]}x{rgb.shape[0]})")


if __name__ == "__main__":
    main()
