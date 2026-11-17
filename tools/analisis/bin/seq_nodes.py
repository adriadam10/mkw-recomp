"""MKW's global sequence nodes (AIGlobalSeq*, the game's phases: TitleMenuMain, StartIntroMovie, WaitRaceStart...):
for each, the 12-byte virtual that returns its name and the vtable holding it (from main's R_AARCH64_RELATIVE
relocations), and the methods every node vtable shares (inherited from the base class). The runtime's sequence watch
(MKW_SEQ_WATCH, runtime/core/diag/seqwatch.c) reads build/seq_nodes.tsv (1.0.0) or build/seq_nodes-<version>.tsv
(build/seq_nodes-180.tsv for 1.8.0) to name the node a game thread is in. The name getter is adrp/add/ret: 12 bytes
in 1.0.0, 16 in 1.8.0 (padded with an udf), so both sizes count.

Usage: uv run tools/analisis/bin/seq_nodes.py [--shared]   (1.0.0: game/, build/bindb.sqlite -> build/seq_nodes.tsv)
       uv run tools/analisis/bin/seq_nodes.py --game ~/Juegos/mkw-copias/1.8.0 --db build/bindb-180.sqlite \
           --out build/seq_nodes-180.tsv
       --shared prints the vtable slots every node shares (seqwatch.c's SLOT_* must match them)
"""

import argparse
import os
import sqlite3
import sys
from collections import Counter
from pathlib import Path

sys.path[:0] = ["recomp", "tools/gpu"]
from module import Module
from nso import R_AARCH64_RELATIVE

BASE = 0x7000000000


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shared", action="store_true", help="print the vtable slots every node shares")
    ap.add_argument("--game", type=Path, default=Path(os.environ.get("MKW_GAME", "game")), help="the version's dir")
    ap.add_argument("--db", default="build/bindb.sqlite", help="that version's bindb.py database")
    ap.add_argument("--out", type=Path, default=Path("build/seq_nodes.tsv"))
    args = ap.parse_args()
    db = sqlite3.connect(args.db)
    rows = db.execute(
        "select s.text, f.addr from str s join ref r on r.target = s.addr join func f on f.addr = r.func "
        "where s.module = 'main' and s.text like 'AIGlobalSeq%' and f.end - f.addr in (12, 16)"
    ).fetchall()
    getters = {addr - BASE: text.removeprefix("AIGlobalSeq") for text, addr in rows}
    m = Module("main", args.game / "exefs/main", 0)
    rel = {off: add for off, typ, _, add in m.nso.relocs if typ == R_AARCH64_RELATIVE}
    slot_of = {add: off for off, add in rel.items() if add in getters}
    out = ["# getter_off\tname_slot_off\tname (main offsets; tools/analisis/bin/seq_nodes.py)"]
    shared = Counter()
    for g, name in sorted(getters.items(), key=lambda kv: kv[1]):
        s = slot_of.get(g)
        out.append(f"{g:#x}\t{s:#x}\t{name}" if s else f"{g:#x}\t-\t{name}")
        if s:
            for k in range(-17, 22):
                v = rel.get(s + 8 * k)
                if v is not None:
                    shared[(k, v)] += 1
    args.out.write_text("\n".join(out) + "\n")
    print(f"{len(getters)} nodes, {len(slot_of)} with a vtable slot -> {args.out}")
    if args.shared:
        n = len(slot_of)
        for (k, v), c in sorted(shared.items()):
            if c >= n * 0.9:
                print(f"slot {k:+d} from the name: {v:#x} in {c} of {n} vtables")


if __name__ == "__main__":
    main()
