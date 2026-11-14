"""Reference implementation of the Gray-Scott demonstrator (docs/gray-scott.md).

Plain numpy, one array per field, no split of the work: what the distributed
C++ pipeline must agree with, within the tolerances of the specification.
"""

import numpy as np

_GOLDEN = np.uint64(0x9E3779B97F4A7C15)
_MIX1 = np.uint64(0xBF58476D1CE4E5B9)
_MIX2 = np.uint64(0x94D049BB133111EB)
BINS = 64


def noise(seed, j):
    """r(j) of the specification for an array of uint64 indices."""
    with np.errstate(over="ignore"):
        z = np.uint64(seed) + (j + np.uint64(1)) * _GOLDEN
        z = (z ^ (z >> np.uint64(30))) * _MIX1
        z = (z ^ (z >> np.uint64(27))) * _MIX2
        z = z ^ (z >> np.uint64(31))
    return (z >> np.uint64(11)).astype(np.float64) * 2.0**-53


def initial(p):
    w, h = p["width"], p["height"]
    u = np.ones((h, w))
    v = np.zeros((h, w))
    side = max(1, min(w, h) // 8)
    x0, y0 = (w - side) // 2, (h - side) // 2
    u[y0 : y0 + side, x0 : x0 + side] = 0.5
    v[y0 : y0 + side, x0 : x0 + side] = 0.25
    i = np.arange(h * w, dtype=np.uint64).reshape(h, w)
    u = u + 0.02 * (noise(p["seed"], np.uint64(2) * i) - 0.5)
    v = v + 0.02 * (noise(p["seed"], np.uint64(2) * i + np.uint64(1)) - 0.5)
    return np.clip(u, 0.0, 1.0), np.clip(v, 0.0, 1.0)


def step(u, v, p):
    def lap(x):
        n, s = np.roll(x, 1, axis=0), np.roll(x, -1, axis=0)
        w, e = np.roll(x, 1, axis=1), np.roll(x, -1, axis=1)
        return (((n + s) + w) + e) - 4.0 * x

    uvv = (u * v) * v
    u2 = u + p["dt"] * ((p["Du"] * lap(u) - uvv) + p["F"] * (1.0 - u))
    v2 = v + p["dt"] * ((p["Dv"] * lap(v) + uvv) - (p["F"] + p["k"]) * v)
    return u2, v2


def histogram(v):
    bins = np.clip(np.floor(v * BINS), 0, BINS - 1).astype(np.int64)
    return np.bincount(bins.ravel(), minlength=BINS)


def simulate(p):
    """Returns (U, V, snapshots); a snapshot is (step, sumU, sumV, histogram)."""
    u, v = initial(p)
    snaps = []
    for s in range(1, p["steps"] + 1):
        u, v = step(u, v, p)
        if s % p["snapshot_every"] == 0 or s == p["steps"]:
            snaps.append((s, float(u.sum()), float(v.sum()), histogram(v)))
    return u, v, snaps
