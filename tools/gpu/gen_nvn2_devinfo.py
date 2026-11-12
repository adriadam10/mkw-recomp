"""Write runtime/gfx/nvn/procs/nvn2_devinfo.inc: the value of every nvnDeviceGetInteger query as MKW's NVN2 driver answers.

Why: the game checks the API version and sizes its pools, alignments and limits from these. The NVN1 front answered
Tegra X1 values (55.15 and friends); MKW requires NVN2's. The driver's query (sdk+0x5b57c0) is a jump table over 0xaa
indices: constants in place, a few fields of its device object, and the rest from a second table (sdk+0x59bf50, a
jump table of constants keyed by index - 2). This walks both tables of the dump's sdk (LAYOUTS: 1.0.0 into nvn2_devinfo.inc, 1.8.0 into
nvn2_devinfo_180.inc); a device field is not known
statically and keeps NVN1's value (its row says which field). Values only, no code.

Usage: uv run tools/gpu/gen_nvn2_devinfo.py
"""

import os
import re
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))

from nso import Nso

OUT = ROOT / "runtime/gfx/nvn/procs/nvn2_devinfo.inc"
# sdk build id -> the two jump tables: (cases, byte table, count, ret, default) of the query, then of the second
# table. 1.0.0: the query sdk+0x5b57c0, the second table sdk+0x59bf50. 1.8.0 (11/11/2026, by disassembly): the query
# sdk+0x65b680 (nvnDeviceGetInteger sdk+0x657050 calls it; cmp #0xa9, adrp/add of the byte table, mov w0, #0xe7; the
# cases at its adr, every one joining at sdk+0x65b814), the second table sdk+0x63f460 (cmp w9, #0xaf: 0xb0 entries,
# mov w8, #0xe, joining at sdk+0x63f5c0).
LAYOUTS = {
    "90ebba182862b9ecdc21fcb87b99901224fb8f72": (
        (0x5B5810, 0xB74B78, 0xAA, 0x5B5928, 0xE7),
        (0x59BF80, 0xB74940, 0xAF, 0x59C0A0, 0xE),
        "nvn2_devinfo.inc",
        "1.0.0",
    ),
    "7a68b13d88183767fb9c7babffe89f3d368752e2": (
        (0x65B6D0, 0xC263A8, 0xAA, 0x65B814, 0xE7),
        (0x63F490, 0xC26120, 0xB0, 0x63F5C0, 0xE),
        "nvn2_devinfo_180.inc",
        "1.8.0",
    ),
}
QUERY_CASES = QUERY_TABLE = QUERY_COUNT = QUERY_RET = QUERY_DEFAULT = 0
TABLE_CASES = TABLE_BYTES = TABLE_COUNT = TABLE_STORE = TABLE_DEFAULT = 0
NVN1 = re.compile(r"\[0x([0-9A-F]+)\] = (-?\d+)")


def first(cs: Cs, img: bytes, at: int, n: int = 2) -> list:
    return list(cs.disasm(img[at : at + 4 * n], at))


def const_of(ins) -> int | None:
    """Value of a `mov w0/w8, #imm` (or wzr), else None."""
    if ins.mnemonic != "mov":
        return None
    src = ins.op_str.split(", ", 1)[1]
    return 0 if src == "wzr" else int(src.lstrip("#"), 0) if src.startswith("#") else None


def table_value(cs: Cs, img: bytes, idx: int) -> int | None:
    """The second table's answer for query idx (it is keyed by idx - 2)."""
    k = idx - 2
    if not 0 <= k <= TABLE_COUNT - 1:
        return None
    t = TABLE_CASES + img[TABLE_BYTES + k] * 4
    if t == TABLE_STORE:
        return TABLE_DEFAULT
    a, b = first(cs, img, t)
    v = const_of(a)
    if v is not None and b.mnemonic == "movk":  # mov w8, #lo; movk w8, #hi, lsl #16
        hi = int(b.op_str.split("#")[1].split(",")[0], 0)
        v |= hi << 16
    return v


def main() -> None:
    global QUERY_CASES, QUERY_TABLE, QUERY_COUNT, QUERY_RET, QUERY_DEFAULT
    global TABLE_CASES, TABLE_BYTES, TABLE_COUNT, TABLE_STORE, TABLE_DEFAULT
    sdk = Nso(str(Path(os.environ.get("MKW_GAME", "game")) / "exefs/sdk"))
    if sdk.build_id not in LAYOUTS:
        raise SystemExit(f"sdk build {sdk.build_id}: its query tables are not in LAYOUTS")
    query, table, name, version = LAYOUTS[sdk.build_id]
    QUERY_CASES, QUERY_TABLE, QUERY_COUNT, QUERY_RET, QUERY_DEFAULT = query
    TABLE_CASES, TABLE_BYTES, TABLE_COUNT, TABLE_STORE, TABLE_DEFAULT = table
    out = OUT.with_name(name)
    img = bytes(sdk.image)
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    src = (Path(__file__).with_name("nvn1_devinfo.txt")).read_text()
    nvn1 = {int(a, 16): int(b) for a, b in NVN1.findall(src)}
    rows, fields = [], 0
    for idx in range(QUERY_COUNT):
        t = QUERY_CASES + img[QUERY_TABLE + idx] * 4
        ins = first(cs, img, t, 1)[0]
        note = ""
        if t == QUERY_RET:
            v = QUERY_DEFAULT
        elif (v := const_of(ins)) is not None and ins.op_str.startswith("w0"):
            pass
        elif ins.mnemonic == "ldur":  # the second table's result
            v = table_value(cs, img, idx)
        else:
            v, note = nvn1.get(idx, 0), f"device field ({ins.mnemonic} {ins.op_str}): NVN1 value"
            fields += 1
        if v is None:
            v, note = nvn1.get(idx, 0), "not decoded: NVN1 value"
        rows.append(f"    [{idx:#04x}] = {v},{'   // ' + note if note else ''}")
    head = f"// NVN2 nvnDeviceGetInteger answers of MKW {version}'s driver (tools/gpu/gen_nvn2_devinfo.py; do not edit).\n"
    out.write_text(head + "\n".join(rows) + "\n")
    print(f"{out.relative_to(ROOT)}: {QUERY_COUNT} queries, {fields} device fields kept from NVN1")


if __name__ == "__main__":
    main()
