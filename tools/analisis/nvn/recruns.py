"""Which stretches of a frame's NVN recording repeat in the next one, with the uniform blocks' addresses left out.

Why: reusing what the replay recorded needs a unit that repeats whole:
the submission does not (the game adds and removes objects every frame), so this measures the candidates. A segment is
the ops up to a draw or dispatch, keyed by their bytes with BIND_UBO's address zeroed (the per-object uniform blocks
come from a linear allocator and move every frame); a pass is the ops from one SET_RT to the next. For B against A it
prints the share of B's draws in passes identical to one of A's, and in runs of consecutive identical segments
(difflib) by run length.
The recordings come from mkw --replay with MKW_CAP_RECS=<dir> (runtime/gfx/cap/cap_replay.c).

Usage: uv run tools/analisis/nvn/recruns.py A.bin B.bin
"""

import argparse
import difflib
from collections import Counter
from pathlib import Path

from recdiff import op_names, ops


def segments(rec: list[tuple[int, bytes]], names: list[str]) -> tuple[list[bytes], list[int]]:
    """Segment keys, and the pass index of each segment."""
    ubo, set_rt = names.index("BIND_UBO"), names.index("SET_RT")
    draws = {i for i, n in enumerate(names) if n.startswith(("DRAW", "DISPATCH"))}
    keys, passes, cur, pas = [], [], [], 0
    for op, p in rec:
        if op == set_rt:
            pas += 1
        if op == ubo:
            p = p[:8] + bytes(8) + p[16:]
        cur.append(op.to_bytes(2, "little") + p)
        if op in draws:
            keys.append(b"".join(cur))
            passes.append(pas)
            cur = []
    return keys, passes


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    args = ap.parse_args()
    names = op_names()
    ka, pa = segments(ops(args.a), names)
    kb, pb = segments(ops(args.b), names)
    n = len(kb)

    def by_pass(keys: list[bytes], passes: list[int]) -> dict[int, list[bytes]]:
        out: dict[int, list[bytes]] = {}
        for k, p in zip(keys, passes, strict=True):
            out.setdefault(p, []).append(k)
        return out

    passes_a = Counter(tuple(v) for v in by_pass(ka, pa).values())
    same_pass = 0
    pass_b = by_pass(kb, pb)
    for v in pass_b.values():
        if passes_a[tuple(v)] > 0:
            passes_a[tuple(v)] -= 1
            same_pass += len(v)
    print(
        f"A {len(ka)} segments in {len(set(pa))} passes, B {n} in {len(pass_b)}; segments equal to one of A's (multiset) "
        f"{100 * sum((Counter(kb) & Counter(ka)).values()) / n:.1f} %; in passes equal to one of A's {100 * same_pass / n:.1f} %"
    )
    sm = difflib.SequenceMatcher(None, ka, kb, autojunk=False)
    runs = sorted((m.size for m in sm.get_matching_blocks() if m.size), reverse=True)
    for at_least in (1, 4, 16, 64, 256):
        k = sum(r for r in runs if r >= at_least)
        print(
            f"  in runs of >= {at_least:>3} identical segments in order: {100 * k / n:5.1f} % ({sum(r >= at_least for r in runs)} runs)"
        )
    print(f"  longest runs: {runs[:12]}")


if __name__ == "__main__":
    main()
