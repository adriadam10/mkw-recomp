"""Disassembly of one function of main, sdk or subsdk0, with call targets and PLT imports named from the binary DB.

Why: the last resort when q.py is not enough (argument meaning, a field offset). One function at a time, with names,
so it costs few tokens.

Usage: uv run tools/analisis/bin/disasm.py MODULE+0xOFFSET [--count N] [--exefs DIR] [--db DB]
The function is the one containing the address (build/bindb.sqlite); --count disassembles N instructions from the
address instead.
"""

import argparse
import os
import sqlite3
import subprocess
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "recomp"))
from nso import Nso


def demangle(names: list[str]) -> list[str]:
    out = subprocess.run(
        ["c++filt"], input="\n".join(names), capture_output=True, text=True, check=True
    ).stdout.splitlines()
    return out if len(out) == len(names) else names


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addr")
    ap.add_argument("--count", type=int)
    ap.add_argument("--exefs", type=Path, default=Path(os.environ.get("MKW_GAME", "game")) / "exefs")
    ap.add_argument("--db", type=Path, default=ROOT / "build/bindb.sqlite", help="1.8.0: build/bindb-180.sqlite")
    a = ap.parse_args()
    mod, off = a.addr.split("+")
    off = int(off, 16)
    db = sqlite3.connect(a.db)
    base = {m: b for m, b in db.execute("SELECT name, base FROM module")}
    addr = base[mod] + off
    if a.count:
        lo, hi = addr, addr + 4 * a.count
    else:
        lo, hi = db.execute(
            "SELECT addr, end FROM func WHERE addr<=? AND end>? ORDER BY addr DESC LIMIT 1", (addr, addr)
        ).fetchone()
    names = {}
    for t, n in db.execute("SELECT addr, name FROM plt WHERE name IS NOT NULL"):
        names[t] = n
    for t, n in db.execute("SELECT addr, name FROM func WHERE name IS NOT NULL AND name != ''"):
        names.setdefault(t, n)
    nso = Nso(str(a.exefs / mod))
    img = bytes(nso.image)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    rows = list(md.disasm(img[lo - base[mod] : hi - base[mod]], lo))
    targets = {}
    for i in rows:
        if i.mnemonic in ("bl", "b") and i.op_str.startswith("#"):
            t = int(i.op_str[1:], 16)
            if t in names:
                targets[t] = names[t]
    dem = dict(zip(targets, demangle(list(targets.values()))))
    print(f"{mod}+{lo - base[mod]:#x}..{hi - base[mod]:#x}")
    for i in rows:
        note = ""
        if i.mnemonic in ("bl", "b") and i.op_str.startswith("#"):
            t = int(i.op_str[1:], 16)
            m = next((k for k, v in base.items() if v <= t < v + 0x10000000), mod)
            note = f"  ; {m}+{t - base[m]:#x} {dem.get(t, '')}"
        print(f"  {i.address - base[mod]:#9x}  {i.mnemonic:8} {i.op_str}{note}")


if __name__ == "__main__":
    main()
