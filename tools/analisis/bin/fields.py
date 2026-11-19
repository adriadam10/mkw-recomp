"""Who reads or writes a field, per function, from the binary alone: direct accesses [R, #OFF], the fields of a pointer
loaded from [R, #VIA], or of the pointer a virtual call through vtable slot offset VCALL returns.

Why: a census of the consumers of a value (the ModuleSystem's step record and its interval, fps-libres) has to see the
whole game, not the code one run happens to execute (Adrià 19/11/2026: censuses are static; the perf watchpoints of
lhzwp/lhzwp2 missed every writer). The pointer is followed inside one function only: through register copies, until
the register is redefined or a call clobbers it (x0-x18); spills to the stack and copies into other objects are not
followed, so a consumer that receives the value as an argument shows up as a caller of one listed here.

Usage: uv run tools/analisis/bin/fields.py --off 0x50c [--off 0x214]              direct [R, #OFF]
       uv run tools/analisis/bin/fields.py --via 0x58 --field 0 --field 4 --field 8   fields of P = [R, #0x58]
       uv run tools/analisis/bin/fields.py --vcall 0x1a8 --field 8                  fields of x0 after blr via [vt, #0x1a8]
       [--list] every site; [--exefs DIR] [--db DB] (1.8.0: --db build/bindb-180.sqlite --exefs .../1.8.0/exefs)
"""

import argparse
import os
import sqlite3
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
from capstone.arm64 import ARM64_OP_MEM, ARM64_OP_REG

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "recomp"))
from nso import Nso  # noqa: E402

# load/store (unsigned immediate) opcodes by size: (mask 0xFFC00000 value, bytes)
LDST = {
    0xF9400000: 8,
    0xF9000000: 8,
    0xB9400000: 4,
    0xB9000000: 4,
    0xB9800000: 4,
    0xBD400000: 4,
    0xBD000000: 4,
    0xFD400000: 8,
    0xFD000000: 8,
    0x79400000: 2,
    0x79000000: 2,
    0x39400000: 1,
    0x39000000: 1,
}
CLOBBER = {f"x{i}" for i in range(19)}


def reg64(name: str) -> str:
    return "x" + name[1:] if name[0] in "wx" and name[1:].isdigit() else name


def candidates(words: np.ndarray, offs: list[int], only64: bool = False) -> np.ndarray:
    """Word indexes of LDR/STR [Rn, #off] with an unsigned immediate equal to one of offs."""
    op = words & 0xFFC00000
    imm = (words >> 10) & 0xFFF
    hit = np.zeros(len(words), dtype=bool)
    for opcode, size in LDST.items():
        if only64 and opcode != 0xF9400000:
            continue
        for off in offs:
            if off % size == 0:
                hit |= (op == opcode) & (imm == off // size)
    return np.nonzero(hit)[0]


def mem_access(ins) -> tuple[str, int, str] | None:
    """(base register, displacement, 'ld'/'st' + size) of a load or store, or None."""
    m = ins.mnemonic
    if not (m.startswith("ld") or m.startswith("st")) or m.startswith(("ldx", "stx", "lda", "stl")):
        return None
    mems = [o for o in ins.operands if o.type == ARM64_OP_MEM]
    if not mems or ins.writeback:
        return None
    base = reg64(ins.reg_name(mems[0].mem.base))
    return base, mems[0].mem.disp, f"{m} {ins.reg_name(ins.operands[0].reg)}"


def defs(ins) -> list[str]:
    """Registers an instruction writes (64-bit names)."""
    _, written = ins.regs_access()
    return [reg64(ins.reg_name(r)) for r in written]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--off", type=lambda s: int(s, 0), action="append", default=[])
    ap.add_argument("--via", type=lambda s: int(s, 0))
    ap.add_argument("--vcall", type=lambda s: int(s, 0))
    ap.add_argument("--field", type=lambda s: int(s, 0), action="append", default=[])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--exefs", type=Path, default=Path(os.environ.get("MKW_GAME", "game")) / "exefs")
    ap.add_argument("--db", type=Path, default=ROOT / "build/bindb.sqlite")
    a = ap.parse_args()
    db = sqlite3.connect(a.db)
    base, text_lo, text_hi = db.execute("SELECT base, text_lo, text_hi FROM module WHERE name='main'").fetchone()
    img = bytes(Nso(str(a.exefs / "main")).image)
    words = np.frombuffer(img[text_lo - base : (text_hi - base) // 4 * 4], dtype="<u4")
    funcs = db.execute("SELECT addr, end FROM func WHERE module='main' ORDER BY addr").fetchall()
    starts = np.array([f[0] for f in funcs], dtype=np.int64)
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cs.detail = True

    def func_of(addr: int) -> tuple[int, int] | None:
        k = int(np.searchsorted(starts, addr, side="right")) - 1
        return funcs[k] if k >= 0 and funcs[k][1] > addr else None

    seed = a.off or [a.via if a.via is not None else a.vcall]
    fs = {func_of(text_lo + 4 * int(i)) for i in candidates(words, seed, only64=not a.off)} - {None}
    sites: dict[int, list[tuple[int, str]]] = defaultdict(list)
    for f0, f1 in sorted(fs):
        code = img[f0 - base : f1 - base]
        tracked: set[str] = set()  # registers holding the followed pointer
        vslot: set[str] = set()  # registers holding a function pointer loaded from [vt, #vcall]
        for ins in cs.disasm(code, f0):
            acc = mem_access(ins)
            if a.off:
                if acc and acc[1] in a.off:
                    sites[f0].append((ins.address, f"{acc[2]}, [{acc[0]}, #{acc[1]:#x}]"))
                continue
            if acc and acc[0] in tracked and acc[1] in a.field:
                sites[f0].append((ins.address, f"{acc[2]}, [P, #{acc[1]:#x}]"))
            new_t: set[str] = set()
            if a.via is not None and acc and acc[2].startswith("ldr x") and acc[1] == a.via:
                new_t = {reg64(ins.reg_name(ins.operands[0].reg))}
            if a.vcall is not None and acc and acc[2].startswith("ldr x") and acc[1] == a.vcall:
                vslot.add(reg64(ins.reg_name(ins.operands[0].reg)))
            if ins.mnemonic == "mov" and len(ins.operands) == 2 and ins.operands[1].type == ARM64_OP_REG:
                if reg64(ins.reg_name(ins.operands[1].reg)) in tracked:
                    new_t = {reg64(ins.reg_name(ins.operands[0].reg))}
            called_v = (
                ins.mnemonic in ("blr", "blraa", "blraaz", "blrab", "blrabz")
                and reg64(ins.reg_name(ins.operands[0].reg)) in vslot
            )
            if ins.mnemonic.startswith("bl"):
                tracked -= CLOBBER
                vslot -= CLOBBER
            for r in defs(ins):
                tracked.discard(r)
                if not (a.vcall is not None and acc and acc[1] == a.vcall):
                    vslot.discard(r)
            tracked |= new_t
            if called_v:
                tracked.add("x0")
    per_field: dict[str, int] = defaultdict(int)
    for f0, lst in sites.items():
        for k in {s.split(", ")[-1] + " " + s.split()[0] for _, s in lst}:
            per_field[k] += 1
    print(f"{len(sites)} functions;", ", ".join(f"{k}: {n}" for k, n in sorted(per_field.items())))
    for f0, lst in sorted(sites.items()):
        print(f"  main+{f0 - base:#x}  {len(lst)}")
        if a.list:
            for addr, s in lst:
                print(f"      main+{addr - base:#x}  {s}")


if __name__ == "__main__":
    main()
