#!/usr/bin/env python3
"""Two serial replays of one capture compared draw by draw: what each draw cost in A and in B, summed per submission
and by what B did with it (MKW_REUSE: reused, captured, replayed as usual).

Why: between two replays the frame CPU moves ±3 ms (ru20/ru21), more than a change per draw. The same draw at the same
op position of the same submission, timed in both, gives the change for exactly the draws it touches (grabacion-
reutilizada, 08/11/2026: the reused draws cost 1.4-3.7 us replayed as usual, so a hit at 3.4-4.2 us loses).

The files come from `build/mkw --replay` with MKW_TRACE=draw MKW_OP_DUMP=FILE (runtime/gfx/vk/exec/vk_exec.c): one
line per draw or dispatch, "submission op-position kind ticks". Kinds above the NVN ops are vk_exec.c's OPX_* (the
reused and captured ones by default 54 and 55: OP_COUNT_ + 3 and + 4).

Usage: uv run tools/analisis/nvn/opjoin.py A.txt B.txt [--hit 54] [--cap 55] [--tsc 3293.8]
"""

import argparse
from collections import defaultdict


def load(path: str) -> dict[tuple[int, int], tuple[int, int]]:
    d = {}
    with open(path) as fh:
        for line in fh:
            f = line.split()
            if len(f) != 4:
                continue  # the last line, cut when the process exits
            s, pos, k, t = map(int, f)
            d[(s, pos)] = (k, t)
    return d


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--hit", type=int, default=54, help="kind of a reused draw in B")
    ap.add_argument("--cap", type=int, default=55, help="kind of a captured draw in B")
    ap.add_argument("--tsc", type=float, default=3293.8, help="TSC ticks per us (dmesg: tsc: Refined ... MHz)")
    args = ap.parse_args()
    a, b = load(args.a), load(args.b)
    names = {args.hit: "reused", args.cap: "captured"}
    agg: dict[tuple[int, str], list[float]] = defaultdict(lambda: [0, 0.0, 0.0])
    missing = 0
    for key, (k, t) in b.items():
        if key not in a:
            missing += 1
            continue
        row = agg[(key[0], names.get(k, "other"))]
        row[0] += 1
        row[1] += a[key][1] / args.tsc
        row[2] += t / args.tsc
    print(f"draws of B not in A: {missing}")
    print("sub class        n   A ms    B ms  A us/draw B us/draw")
    for s in sorted({s for s, _ in agg}):
        for cls in ("reused", "captured", "other"):
            if (s, cls) in agg:
                n, ta, tb = agg[(s, cls)]
                print(f"{s:3d} {cls:9s} {int(n):5d} {ta / 1e3:7.2f} {tb / 1e3:7.2f} {ta / n:9.2f} {tb / n:9.2f}")


if __name__ == "__main__":
    main()
