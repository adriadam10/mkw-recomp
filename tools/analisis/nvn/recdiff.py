"""Compares two NVN recordings op by op: how much of a frame repeats in the next one.

Why: the replay turns each submission's ops into Vulkan anew every frame (7-8 us a draw). The game sends a race frame
as one ~2 MB recording whose bytes differ every frame (MKW_CAP_SUBLIST); whether the ops and their state repeat with
only a few fields changing decides whether the replay's work can be kept from one frame to the next.
The recordings come from mkw --replay with MKW_CAP_RECS=<dir> (runtime/gfx/cap/cap_replay.c). Ops are aligned by type
(difflib); of the aligned pairs it counts the identical ones and, for the others, which 8-byte words of the payload
changed, per op type.

Usage: uv run tools/analisis/nvn/recdiff.py A.bin B.bin [--words N]
"""

import argparse
import difflib
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def op_names() -> list[str]:
    text = (ROOT / "runtime/gfx/nvn/api/nvn_obj.h").read_text()
    body = re.search(r"enum \{\s*(OP_END,.*?OP_COUNT_)", text, re.DOTALL)
    if not body:
        raise SystemExit("recdiff: the op enum was not found in nvn_obj.h")
    return [n.strip()[3:] for n in re.sub(r"//[^\n]*", "", body[1]).split(",") if n.strip()]


def ops(path: Path) -> list[tuple[int, bytes]]:
    data = path.read_bytes()
    out, at = [], 0
    while at + 8 <= len(data):
        op, size = struct.unpack_from("<II", data, at)
        if size < 8 or size & 7 or at + size > len(data):
            raise SystemExit(f"recdiff: {path}: bad op header at {at:#x} (op {op}, size {size})")
        out.append((op, data[at + 8 : at + size]))
        at += size
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("--words", type=int, default=4, help="changed payload words shown per op type")
    args = ap.parse_args()
    names = op_names()
    a, b = ops(args.a), ops(args.b)
    sm = difflib.SequenceMatcher(None, [o for o, _ in a], [o for o, _ in b], autojunk=False)
    pairs = same = 0
    per_type: dict[int, Counter] = defaultdict(Counter)
    words: dict[int, Counter] = defaultdict(Counter)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != "equal":
            continue
        for k in range(i2 - i1):
            op, pa = a[i1 + k]
            pb = b[j1 + k][1]
            pairs += 1
            if pa == pb:
                same += 1
                per_type[op]["same"] += 1
                continue
            per_type[op]["diff"] += 1
            for w in range(0, min(len(pa), len(pb)), 8):
                if pa[w : w + 8] != pb[w : w + 8]:
                    words[op][w] += 1
    draws = sum(c["same"] + c["diff"] for op, c in per_type.items() if names[op].startswith(("DRAW", "DISPATCH")))
    print(
        f"{args.a.name}: {len(a)} ops; {args.b.name}: {len(b)} ops; aligned {pairs} "
        f"({100 * pairs / max(len(a), 1):.1f} % of A), identical {same} ({100 * same / max(pairs, 1):.1f} % of aligned); "
        f"aligned draws and dispatches {draws}"
    )
    print(f"{'op':<16}{'aligned':>9}{'identical':>10}  changed words (byte offset: pairs)")
    for op, c in sorted(per_type.items(), key=lambda kv: -(kv[1]["same"] + kv[1]["diff"])):
        n = c["same"] + c["diff"]
        top = ", ".join(f"+{w}: {k}" for w, k in words[op].most_common(args.words))
        print(f"{names[op]:<16}{n:>9}{100 * c['same'] / n:>9.1f}%  {top}")


if __name__ == "__main__":
    main()
