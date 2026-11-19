"""The game's phase table, seq_nodes-<version>.tsv, written into the game folder from exefs/main alone.

Why: Settings > Hacks > Straight to (MKW_QUICK_RACE) presses through the menus by phase, and the runtime's sequence
watch (runtime/core/diag/seqwatch.c) learns the phases from this table. Only tools/analisis/bin/seq_nodes.py made it,
from the 147 MB executable database of tools/analisis/bin/bindb.py, into a source checkout's build/: a player's setup
never had one, the watch logged "watch off" and not one press of Straight to fired (18/11/2026, 1.8.0 on Windows).

How: every phase class has a name getter, `adrp x0; add x0, x0; ret` (12 bytes in 1.0.0, 16 in 1.8.0 with an udf
after it) returning "AIGlobalSeq<Name>", and its vtable holds it through an R_AARCH64_RELATIVE relocation. The
relocations that point at such a getter give each name's slot, as seq_nodes.py finds them through the database: the
same lines for every node with a slot (checked on 1.8.0, 18/11/2026: 285 of 285), without the getters no vtable holds,
which the runtime skips anyway. Reads main in about a second.

Usage: python -I seqnodes.py --exefs DIR --out FILE   (prepare.py runs it in its cleanup step)
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "recomp"))
from nso import R_AARCH64_RELATIVE, Nso  # noqa: E402

PREFIX = b"AIGlobalSeq"
RET = 0xD65F03C0


def adrp_add_x0(img: bytes, at: int) -> int | None:
    """The address `adrp x0, P; add x0, x0, #imm; ret` at `at` returns, else None."""
    w0, w1, w2 = (int.from_bytes(img[at + 4 * i : at + 4 * i + 4], "little") for i in range(3))
    if (w0 & 0x9F00001F) != 0x90000000 or (w1 & 0xFFC003FF) != 0x91000000 or w2 != RET:
        return None
    imm = ((w0 >> 5) & 0x7FFFF) << 2 | (w0 >> 29) & 3
    if imm & 1 << 20:
        imm -= 1 << 21
    return (at & ~0xFFF) + (imm << 12) + ((w1 >> 10) & 0xFFF)


def nodes(main: Path) -> list[tuple[int, int, str]]:
    """(getter, slot, name) for every phase whose getter a vtable holds, sorted by name as seq_nodes.py writes them."""
    nso = Nso(str(main))
    img, text_lo, text_hi = nso.image, nso.text[0], nso.text[0] + nso.text[1]
    slot_of: dict[int, int] = {}
    names: dict[int, str] = {}
    for off, typ, _, add in nso.relocs:
        if typ != R_AARCH64_RELATIVE or not text_lo <= add < text_hi - 12:
            continue
        if add not in names:
            s = adrp_add_x0(img, add)
            if s is None or not 0 <= s < len(img) or img[s : s + len(PREFIX)] != PREFIX:
                continue
            names[add] = nso.cstr(s).removeprefix("AIGlobalSeq")
        slot_of[add] = off  # the last relocation wins, as in seq_nodes.py
    return sorted(((g, slot_of[g], n) for g, n in names.items()), key=lambda r: r[2])


def write(exefs: Path, out: Path) -> int:
    """The table for exefs/main into out (through a .part file, so a cut setup leaves no half table); its rows."""
    rows = nodes(exefs / "main")
    lines = ["# getter_off\tname_slot_off\tname (main offsets; tools/prepare/seqnodes.py)"]
    lines += [f"{g:#x}\t{s:#x}\t{n}" for g, s, n in rows]
    tmp = out.with_suffix(".part")
    tmp.write_text("\n".join(lines) + "\n")
    tmp.replace(out)
    return len(rows)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exefs", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    print(f"{write(args.exefs, args.out)} phases -> {args.out}")


if __name__ == "__main__":
    main()
