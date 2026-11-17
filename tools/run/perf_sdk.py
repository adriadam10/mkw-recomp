"""How much CPU the SDK's recompiled code takes in a perf profile of build/mkw, by thread and by namespace.

Why: deciding whether an SDK namespace (nn::os, libc, math) is worth replacing by native code needs its share of the
game threads' time, not its call count (the census of 05/11/2026 found 23 M LockMutex calls in a race, which says
nothing of their cost). Lifted functions are f_<guest address>; bindb names them and gives their module.

Usage: uv run tools/run/perf_sdk.py scratchpad/NAME.perf [--threads 8] [--top 25]
       (a perf record of build/mkw, as run_perf.sh and race.sh leave; needs build/bindb.sqlite)
"""

import argparse
import bisect
import re
import sqlite3
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/analisis/runtime"))
from sdk_gaps import namespace

LINE = re.compile(r"^\s*([\d.]+)%\s*\|([^|]*)\|\[.\]\s+(\S+)")  # user-space samples; "-t |" fields
GUEST = re.compile(r"^f_([0-9a-f]+)")


def load_funcs(db: Path) -> tuple[list[int], list[tuple[str, str]]]:
    con = sqlite3.connect(db)
    rows = sorted(con.execute("SELECT addr, module, coalesce(name, '') FROM func"))
    return [r[0] for r in rows], [(r[1], r[2]) for r in rows]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("perf", type=Path)
    ap.add_argument("--db", type=Path, default=ROOT / "build/bindb.sqlite")
    ap.add_argument("--threads", type=int, default=8, help="threads to show, busiest first")
    ap.add_argument("--top", type=int, default=25, help="hottest SDK functions to list")
    args = ap.parse_args()
    out = subprocess.run(
        ["perf", "report", "-i", str(args.perf), "--stdio", "--no-children", "--sort", "comm,symbol", "-t", "|"],
        capture_output=True, text=True, check=True,
    ).stdout  # fmt: skip
    starts, info = load_funcs(args.db)
    per_thread: dict[str, Counter] = defaultdict(Counter)
    sdk_fn: Counter = Counter()
    for line in out.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        pct, comm, sym = float(m.group(1)), m.group(2).strip(), m.group(3)
        g = GUEST.match(sym)
        if not g:
            per_thread[comm]["runtime and libraries"] += pct
            continue
        i = bisect.bisect_right(starts, int(g.group(1), 16)) - 1
        mod, name = info[i] if i >= 0 else ("?", "")
        if mod == "main":
            per_thread[comm]["game (main)"] += pct
            continue
        ns = namespace(name) if name else f"{mod} (no name)"
        per_thread[comm][f"{mod} {ns}"] += pct
        sdk_fn[f"{mod} {name or hex(starts[i])}"] += pct
    total = Counter()
    for c in per_thread.values():
        total.update(c)
    busiest = sorted(per_thread, key=lambda t: -sum(per_thread[t].values()))[: args.threads]
    print("% of all samples, by thread; inside it, by owner (SDK by namespace)")
    for t in busiest:
        c = per_thread[t]
        sdk = sum(v for k, v in c.items() if k.startswith(("sdk", "subsdk0")))
        print(f"{sum(c.values()):6.2f}%  {t}: game {c['game (main)']:.2f}, SDK {sdk:.2f}, runtime and libraries "
              f"{c['runtime and libraries']:.2f}")  # fmt: skip
        for k, v in sorted(c.items(), key=lambda kv: -kv[1])[:6]:
            if k.startswith(("sdk", "subsdk0")) and v >= 0.05:
                print(f"           {v:6.2f}%  {k}")
    sdk_total = sum(v for k, v in total.items() if k.startswith(("sdk", "subsdk0")))
    print(f"all threads: SDK {sdk_total:.2f}%, game {total['game (main)']:.2f}%")
    print("-- hottest SDK functions (% of all samples)")
    for k, v in sdk_fn.most_common(args.top):
        print(f"{v:6.2f}%  {k[:140]}")


if __name__ == "__main__":
    main()
