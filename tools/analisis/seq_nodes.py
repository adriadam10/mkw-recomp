"""MKW's global sequence nodes (AIGlobalSeq*, the game's phases: TitleMenuMain, StartIntroMovie, WaitRaceStart...):
for each, the 12-byte virtual that returns its name and the vtable holding it (from main's R_AARCH64_RELATIVE
relocations), and the methods every node vtable shares (inherited from the base class). The runtime's sequence watch
(MKW_SEQ_WATCH, runtime/core/diag/seqwatch.c) reads build/seq_nodes.tsv to name the node a game thread is in.

Usage: uv run tools/analisis/seq_nodes.py [--shared]   (writes build/seq_nodes.tsv; --shared prints the shared slots)
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
    args = ap.parse_args()
    db = sqlite3.connect("build/bindb.sqlite")
    rows = db.execute(
        "select s.text, f.addr from str s join ref r on r.target = s.addr join func f on f.addr = r.func "
        "where s.module = 'main' and s.text like 'AIGlobalSeq%' and f.end - f.addr in (12, 16)"
    ).fetchall()
    getters = {addr - BASE: text.removeprefix("AIGlobalSeq") for text, addr in rows}
    m = Module("main", Path(os.environ.get("MKW_GAME", "game")) / "exefs/main", 0)
    rel = {off: add for off, typ, _, add in m.nso.relocs if typ == R_AARCH64_RELATIVE}
    slot_of = {add: off for off, add in rel.items() if add in getters}
    out = ["# getter_off\tname_slot_off\tname (main offsets; tools/analisis/seq_nodes.py)"]
    shared = Counter()
    for g, name in sorted(getters.items(), key=lambda kv: kv[1]):
        s = slot_of.get(g)
        out.append(f"{g:#x}\t{s:#x}\t{name}" if s else f"{g:#x}\t-\t{name}")
        if s:
            for k in range(-17, 22):
                v = rel.get(s + 8 * k)
                if v is not None:
                    shared[(k, v)] += 1
    Path("build/seq_nodes.tsv").write_text("\n".join(out) + "\n")
    print(f"{len(getters)} nodes, {len(slot_of)} with a vtable slot -> build/seq_nodes.tsv")
    if args.shared:
        n = len(slot_of)
        for (k, v), c in sorted(shared.items()):
            if c >= n * 0.9:
                print(f"slot {k:+d} from the name: {v:#x} in {c} of {n} vtables")


if __name__ == "__main__":
    main()
