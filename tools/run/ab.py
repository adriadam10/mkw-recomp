"""In-launch A/B summary: the perf lines of a run with MKW_AB=<seconds> (runtime/core/diag/perf.c rt_ab), A against B.

Why: two runs of the same build differ by up to 12 ms per frame in a race (rt-j4 37 ms, rt-k0 49 ms, 01/11/2026), more
than most changes. With MKW_AB the blocks of A and B alternate (A B B A) inside one race, so desktop load and scene
weigh the same on both. Only 60-frame windows wholly A ("ab 0/60") or wholly B ("ab 60/60") count.
The race's stretches are not equally heavy, so blocks of 6 s can still favour one side (sl-ab1/sl-ab2i, 06/11/2026:
+0.77 and -1.68 ms for the same change). Given runs with MKW_AB_INVERT=1 too, the means of B - A over the plain runs
and over the inverted ones give the change (their mean) and the scene (half their difference). The race's first window
is left out: it holds the start's stall (33-44 ms, sl-ab1/sl-ab3), wholly A in plain runs and mixed in inverted ones.
A switch that changes the frame rate (MKW_FPS=auto's) is judged on game speed and time at 30 fps or slower too: the
game's speed lines end with each side's sums in races (game.c speed_note), the last one printed here.

Usage: uv run tools/run/ab.py build/run/NAME.log [...]
"""

import re
import statistics
import sys
from pathlib import Path

LINE = re.compile(r"interval avg ([\d.]+) ms.*replay avg ([\d.]+) ms; ab (\d+)/(\d+); world (-?[\d.]+) s")
GPU = re.compile(r"perf: frame \d+: gpu avg ([\d.]+) ms")   # the window's second line (MKW_TRACE=perf with timestamps)
SIDES = re.compile(r"; in races A ([\d.]+) s game ([\d.]+) s at 30 or slower ([\d.]+) s, "
                   r"B ([\d.]+) s game ([\d.]+) s at 30 or slower ([\d.]+) s")


def speeds(text: str) -> list[str]:
    """Each side's game speed and share of time at 30 fps or slower in races, from the last speed line's sums."""
    last = None
    for last in SIDES.finditer(text):
        pass
    if not last:
        return []
    v = [float(x) for x in last.groups()]
    out, speed = [], {}
    for k, (real, game, slow) in (("A", v[0:3]), ("B", v[3:6])):
        speed[k] = game / real if real else 0.0
        out.append(f"  {k} in races: {real:.1f} s, game speed {speed[k]:.5f}x, {slow / real * 100 if real else 0:.1f} % "
                   f"of the time at 30 fps or slower")
    out.append(f"  B - A: game speed {speed['B'] - speed['A']:+.5f}x")
    return out


def summary(path: Path) -> tuple[str, tuple[float, float] | None, bool]:
    side: dict[str, list[tuple[float, float]]] = {"A": [], "B": []}
    gpu: dict[str, list[float]] = {"A": [], "B": []}
    last = ""   # the side of the window line just read: its gpu line follows it
    text = path.read_text(errors="replace")
    inverted = "(MKW_AB_INVERT)" in text
    started = False
    for line in text.splitlines():
        g = GPU.search(line)
        if g and last:
            gpu[last].append(float(g[1]))
            last = ""
            continue
        m = LINE.search(line)
        if not m or m[5].startswith("-"):   # before the race: world -1 us prints as -0.0
            continue
        if not started:   # the race's first window: the start's stall
            started, last = True, ""
            continue
        b, n = int(m[3]), int(m[4])
        last = "A" if b == 0 else "B" if b == n else ""
        if last:
            side[last].append((float(m[1]), float(m[2])))
    out = [path.stem + (" (inverted)" if inverted else "")]
    for k, v in side.items():
        if not v:
            out.append(f"  {k}: no whole window")
            continue
        frame = [x[0] for x in v]
        replay = [x[1] for x in v]
        out.append(f"  {k}: {len(v)} windows, frame {statistics.mean(frame):.2f} ms (median "
                   f"{statistics.median(frame):.2f}), replay {statistics.mean(replay):.2f} ms"
                   + (f", gpu {statistics.mean(gpu[k]):.2f} ms" if gpu[k] else ""))
    delta = None
    if side["A"] and side["B"]:
        a = statistics.mean(x[0] for x in side["A"])
        b = statistics.mean(x[0] for x in side["B"])
        med = statistics.median(x[0] for x in side["B"]) - statistics.median(x[0] for x in side["A"])
        delta = (b - a, med)
        out.append(f"  B - A: {b - a:+.2f} ms per frame ({(b - a) / a * 100:+.1f} %), median {med:+.2f}")
    out += speeds(text)
    return "\n".join(out), delta, inverted


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    plain, inverted = [], []
    for p in sys.argv[1:]:
        text, delta, inv = summary(Path(p))
        print(text)
        if delta is not None:
            (inverted if inv else plain).append(delta)
    if plain and inverted:   # medians too: one heavy window moves a side's mean
        for k, what in enumerate(("mean", "median")):
            d, i = statistics.mean(x[k] for x in plain), statistics.mean(x[k] for x in inverted)
            print(f"change (B - A, scene balanced, {what}s): {(d + i) / 2:+.2f} ms per frame; scene in the plain runs' "
                  f"B blocks {(d - i) / 2:+.2f} ms ({len(plain)} plain, {len(inverted)} inverted)")


if __name__ == "__main__":
    main()
