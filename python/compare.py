#!/usr/bin/env python3
"""Compares a run of the C++ demonstrator with the Python reference.

    compare.py --run DIR                      # DIR holds result.json, U.bin, V.bin
    compare.py --exec BINARY [--width ...]    # run the C++ binary first, then compare

Tolerances are those of docs/gray-scott.md. Exit status 0 if everything agrees.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gray_scott_ref as ref  # noqa: E402

RTOL, ATOL = 1e-9, 1e-12


def compare(run_dir):
    with open(os.path.join(run_dir, "result.json")) as f:
        result = json.load(f)
    p = result["params"]
    shape = (p["height"], p["width"])
    u = np.fromfile(os.path.join(run_dir, result["fields"]["U"]), dtype=np.float64).reshape(shape)
    v = np.fromfile(os.path.join(run_dir, result["fields"]["V"]), dtype=np.float64).reshape(shape)
    ru, rv, rsnaps = ref.simulate(p)
    ok = True

    for name, got, want in (("U", u, ru), ("V", v, rv)):
        diff = np.abs(got - want)
        bad = diff > ATOL + RTOL * np.abs(want)
        y, x = np.unravel_index(int(np.argmax(diff)), shape)
        print(f"{name}: max |diff| {diff.max():.3e} at cell (x={x}, y={y}); "
              f"{int(bad.sum())} cells outside tolerance")
        ok &= not bad.any()

    snaps = result["snapshots"]
    if [s["step"] for s in snaps] != [s[0] for s in rsnaps]:
        print("snapshot steps differ")
        return False
    hist_bound = max(1, int(0.001 * p["width"] * p["height"]))
    for got, (step, su, sv, hist) in zip(snaps, rsnaps):
        rel_u = abs(got["sum_u"] - su) / max(abs(su), 1e-300)
        rel_v = abs(got["sum_v"] - sv) / max(abs(sv), 1e-300)
        l1 = int(np.abs(np.array(got["histogram"], dtype=np.int64) - hist).sum())
        good = rel_u <= RTOL and rel_v <= RTOL and l1 <= hist_bound
        print(f"step {step}: sumU rel {rel_u:.1e}, sumV rel {rel_v:.1e}, "
              f"histogram L1 {l1} (bound {hist_bound}) {'ok' if good else 'FAIL'}")
        ok &= good
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run")
    ap.add_argument("--exec", dest="binary")
    ap.add_argument("--width", type=int, default=64)
    ap.add_argument("--height", type=int, default=48)
    ap.add_argument("--steps", type=int, default=300)
    ap.add_argument("--every", type=int, default=100)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    if a.binary:
        with tempfile.TemporaryDirectory() as d:
            subprocess.run([a.binary, "--out", d, "--width", str(a.width), "--height", str(a.height),
                            "--steps", str(a.steps), "--every", str(a.every), "--seed", str(a.seed)],
                           check=True)
            ok = compare(d)
    elif a.run:
        ok = compare(a.run)
    else:
        ap.error("need --run or --exec")
    print("agrees with the reference" if ok else "DIFFERS from the reference")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
