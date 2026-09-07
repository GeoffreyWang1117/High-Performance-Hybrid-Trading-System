#!/usr/bin/env python3
"""Copy a benchmark series and make one metric slower in its most recent runs.

A gate that never fires and a gate that always fires look identical on a green
build. The only way to tell them apart is to hand the gate a regression you
planted yourself and require it to notice.

The injection is applied to REAL measured runs rather than to synthetic data, so
what the gate sees is the actual noise of the actual host with one deliberate
step added on top. That is the case it has to survive; a step on top of clean
Gaussian noise is not.

Usage:
  scripts/inject_regression.py <src_dir> <dst_dir> --metric NAME --pct 20 [--last 12]
"""

import argparse
import glob
import json
import os
import shutil
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--metric", required=True)
    ap.add_argument("--pct", type=float, required=True,
                    help="percentage slowdown, e.g. 20 for +20%%")
    ap.add_argument("--last", type=int, default=12,
                    help="how many of the most recent runs to slow down")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.src, "*.json")))
    if not files:
        print(f"error: no .json in {args.src}", file=sys.stderr)
        return 2
    if args.last >= len(files):
        print(f"error: --last {args.last} needs a series longer than {len(files)}; "
              "a step at index 0 is not a step", file=sys.stderr)
        return 2

    os.makedirs(args.dst, exist_ok=True)
    for f in glob.glob(os.path.join(args.dst, "*.json")):
        os.remove(f)

    cut = len(files) - args.last
    factor = 1.0 + args.pct / 100.0
    touched = 0
    for i, f in enumerate(files):
        dst = os.path.join(args.dst, os.path.basename(f))
        if i < cut:
            shutil.copyfile(f, dst)
            continue
        d = json.load(open(f))
        found = False
        for r in d["results"]:
            if r["name"] == args.metric:
                r["cost_ns"] *= factor
                r["min_ns"] *= factor
                # rep_spread_pct is deliberately left alone. A real regression
                # does not make a run internally noisier, which is precisely why
                # the within-run number cannot detect one.
                found = True
        if not found:
            print(f"error: metric {args.metric!r} not in {f}", file=sys.stderr)
            return 2
        json.dump(d, open(dst, "w"), indent=2)
        touched += 1

    print(f"wrote {len(files)} runs to {args.dst}; "
          f"last {touched} have {args.metric} slowed by {args.pct:+.1f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
