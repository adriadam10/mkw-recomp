"""Where a float32 constant is used in a module's code: built with MOVZ/MOVK, an FMOV immediate, or loaded from a data
word that holds it (ADRP+LDR, from build/bindb.sqlite's references), counted per function.

Why: the census of a game constant (1/60 s, 60.0, 1/30) before changing what it means. findf.py finds the data words
only; MKW builds most of its floats in the code (main+0x15a0a8: mov w9, #0x8889; movk w9, #0x3c88, lsl #16).

Usage: uv run tools/analisis/bin/consts.py VALUE... [--module main] [--list] [--exefs DIR] [--db DB]
VALUE is a float (0.0166667, 1/60) or raw bits (0x3c888889). --list prints every site, not only the per-function
counts.
"""

import argparse
import os
import sqlite3
import struct
import sys
from collections import Counter
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "recomp"))
from nso import Nso  # noqa: E402

MOVZ_W, MOVK_W, FMOV_S = 0x52800000, 0x72800000, 0x1E201000


def bits_of(text: str) -> int:
    if text.lower().startswith("0x"):
        return int(text, 16)
    num, _, den = text.partition("/")
    v = float(num) / float(den) if den else float(num)
    return struct.unpack("<I", struct.pack("<f", v))[0]


def fmov_imm8(bits: int) -> int | None:
    """The FMOV (scalar, immediate) imm8 encoding this float32 has, or None."""
    for imm8 in range(256):
        a, b, c = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3F
        exp = ((1 - b) << 7) | ((b * 0x1F) << 2) | (c >> 4)
        if (a << 31 | exp << 23 | (c & 0xF) << 19) == bits:
            return imm8
    return None


def sites(words: np.ndarray, bits: int) -> list[tuple[int, str]]:
    """Word indexes of instructions that make bits in a W register (MOVZ/MOVK) or an S register (FMOV)."""
    lo, hi = bits & 0xFFFF, bits >> 16
    out: list[tuple[int, str]] = []
    op = words & 0xFFE00000                    # sf=0, opc, hw: the 32-bit forms only
    imm = (words >> 5) & 0xFFFF
    if hi:
        for i in np.nonzero(((op == MOVZ_W | 1 << 21) | (op == MOVK_W | 1 << 21)) & (imm == hi))[0]:
            rd = int(words[i]) & 0x1F
            if not lo:
                if (int(words[i]) & 0xFFE00000) == MOVZ_W | 1 << 21:
                    out.append((int(i), "movz"))
                continue
            near = words[max(0, i - 6) : i + 7]   # the low half's MOVZ, a few instructions away, same register
            if any((int(w) & 0xFFE0001F) == (MOVZ_W | rd) and (int(w) >> 5) & 0xFFFF == lo for w in near):
                out.append((int(i), "movz+movk"))
    imm8 = fmov_imm8(bits)
    if imm8 is not None:
        for i in np.nonzero((words & 0xFFFFFFE0) == (FMOV_S | imm8 << 13))[0]:
            out.append((int(i), "fmov"))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("values", nargs="+")
    ap.add_argument("--module", default="main")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--exefs", type=Path, default=Path(os.environ.get("MKW_GAME", "game")) / "exefs")
    ap.add_argument("--db", type=Path, default=ROOT / "build/bindb.sqlite", help="1.8.0: build/bindb-180.sqlite")
    a = ap.parse_args()
    db = sqlite3.connect(a.db)
    base, text_lo, text_hi = db.execute("SELECT base, text_lo, text_hi FROM module WHERE name=?", (a.module,)).fetchone()
    img = bytes(Nso(str(a.exefs / a.module)).image)
    words = np.frombuffer(img[text_lo - base : (text_hi - base) // 4 * 4], dtype="<u4")
    data = np.frombuffer(img[: len(img) // 4 * 4], dtype="<u4")
    funcs = db.execute("SELECT addr, end FROM func WHERE module=? ORDER BY addr", (a.module,)).fetchall()
    starts = np.array([f[0] for f in funcs], dtype=np.int64)

    def func_of(addr: int) -> int | None:
        k = int(np.searchsorted(starts, addr, side="right")) - 1
        return funcs[k][0] if k >= 0 and funcs[k][1] > addr else None

    for text in a.values:
        bits = bits_of(text)
        value = struct.unpack("<f", struct.pack("<I", bits))[0]
        found = [(text_lo + 4 * i, kind) for i, kind in sites(words, bits)]
        for w in np.nonzero(data == bits)[0]:   # data words holding it, and the code that loads them
            target = base + 4 * int(w)
            if text_lo <= target < text_hi:
                continue
            for site, kind in db.execute("SELECT site, kind FROM ref WHERE target=?", (target,)):
                found.append((site, f"load {a.module}+{target - base:#x} ({kind})"))
        per = Counter(func_of(s) for s, _ in found)
        print(f"{text} = {value!r} ({bits:#010x}): {len(found)} sites in {len(per)} functions")
        for f, n in per.most_common():
            print(f"  {a.module}+{f - base:#x}  {n}" if f is not None else f"  (no function)  {n}")
        if a.list:
            for s, kind in sorted(found):
                print(f"    {a.module}+{s - base:#x}  {kind}")


if __name__ == "__main__":
    main()
