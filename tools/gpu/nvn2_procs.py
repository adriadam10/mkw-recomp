"""Address of every NVN2 entry point inside the sdk module, from the driver's own proc table.

Why: the driver exports none of its nvn* functions by name; nvnDeviceGetProcAddress finds them by binary search
(1.0.0: sdk+0x5b5a70; names without the "nvn" prefix, sorted, as u32 offsets from the name pool) and returns the
pointer at the same index (1.0.0: sdk+0x5ffad0, a table of RELATIVE-relocated pointers). Reading both tables names
~700 driver functions, which bindb.py stores so q.py prints them, and which give the size of each opaque object (its
SetDefaults).

Each game version ships its own sdk build, with the tables elsewhere and more procs (1.0.0: 698, 1.8.0: 731). TABLES
holds the offsets of the builds checked by hand; for any other build locate() finds them: the name pool is the block
of identifiers around "QueueFinish", the offset table the longest run of u32s naming them in sorted order, and the
pointer table the run of relocated pointers referenced by code (adrp/add) whose entries are the driver's functions,
not the trampolines through its dispatch table (1.8.0 has both: sdk+0xe146b0 jumps through [sdk+0xfe2790 + 8i],
sdk+0xe45aa8 holds the functions themselves, e.g. ProgramBuilderSetDebugDataHashes = str w1/str x2/ret).

Usage: uv run tools/gpu/nvn2_procs.py [--locate]   (prints name -> sdk+offset and the tables; bindb.py imports procs())
       --locate ignores TABLES, to check a hand-entered row against the search.
"""

import os
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))

from nso import R_AARCH64_RELATIVE, Nso

# sdk build id -> (name pool, name offsets, last index, pointers). The runtime keeps the same rows
# (runtime/core/boot/version.c, sdk_*): change both together.
TABLES = {
    # MKW 1.0.0: adrp/add x25 and x24 and mov w22, #0x2b9 in sdk+0x5b5a70; adrp/add x8 in sdk+0x5ffad0.
    "90ebba182862b9ecdc21fcb87b99901224fb8f72": (0xB74C26, 0xB79B90, 0x2B9, 0xD95168),
    # MKW 1.8.0: the search in sdk+0x65c4xx (mov #0x2da, adrp/add of both tables at sdk+0x65c520); pointers referenced
    # at sdk+0x65bee0 (11/11/2026, by locate() and checked by disassembly).
    "7a68b13d88183767fb9c7babffe89f3d368752e2": (0xC26452, 0xC2B870, 0x2DA, 0xE45AA8),
}
IDENT = re.compile(rb"[A-Za-z0-9_]+")
ANCHOR = b"\0QueueFinish\0"  # a proc name no other string table of the sdk holds


def _name_block(img: bytes) -> dict[int, bytes]:
    """{offset: name} of the identifier block around ANCHOR."""
    at = img.find(ANCHOR)
    if at < 0:
        raise ValueError("sdk: no NVN proc name pool (QueueFinish not found)")
    s = e = at + 1
    while True:
        p = img.rfind(b"\0", 0, s - 1) + 1
        if not IDENT.fullmatch(img[p : s - 1]):
            break
        s = p
    while True:
        q = img.index(b"\0", e)
        if not IDENT.fullmatch(img[e:q]):
            break
        e = q + 1
    out, p = {}, s
    while p < e:
        q = img.index(b"\0", p)
        out[p] = img[p:q]
        p = q + 1
    return out


def _sorted_run(img: bytes, off: int, pool: int, names: dict[int, bytes]) -> int:
    k, prev = 0, b""
    while off + 4 * k + 4 <= len(img):
        nm = names.get(pool + struct.unpack_from("<I", img, off + 4 * k)[0])
        if nm is None or nm <= prev:
            break
        prev, k = nm, k + 1
    return k


def _code_refs(nso: Nso, lo: int, hi: int) -> set[int]:
    """Targets in [lo, hi) of an adrp followed within four instructions by an add on the same register."""
    import numpy as np

    img = bytes(nso.image)
    t0, tl = nso.text
    w = np.frombuffer(img[t0 : t0 + tl], dtype="<u4").astype(np.int64)
    out = set()
    for i in np.nonzero((w & 0x9F000000) == 0x90000000)[0]:
        x, pc = int(w[i]), t0 + 4 * int(i)
        imm = ((x >> 29) & 3) | (((x >> 5) & 0x7FFFF) << 2)
        if imm & (1 << 20):
            imm -= 1 << 21
        page = (pc & ~0xFFF) + (imm << 12)
        if page + 0x1000 <= lo or page >= hi:
            continue
        for j in range(i + 1, min(i + 5, len(w))):
            y = int(w[j])
            if (y & 0xFFC00000) == 0x91000000 and ((y >> 5) & 31) == (x & 31):
                t = page + ((y >> 10) & 0xFFF)
                if lo <= t < hi:
                    out.add(t)
                break
    return out


def _is_trampoline(img: bytes, at: int) -> bool:
    """adrp x8 / add x8 / ldr x1, [x8, #n] / br x1: a jump through the driver's dispatch table."""
    a, b, c, d = struct.unpack_from("<4I", img, at)
    return (a & 0x9F00001F) == 0x90000008 and (b & 0xFFC003FF) == 0x91000108 and (c & 0xFFC003FF) == 0xF9400101 and d == 0xD61F0020


def locate(nso: Nso) -> tuple[int, int, int, int]:
    """(name pool, name offsets, last index, pointers) of an sdk build, found from its contents."""
    img = bytes(nso.image)
    names = _name_block(img)
    lo = min(names)
    ro0, rol = nso.rodata
    best = (0, 0, 0)
    end = (max(names) + len(names[max(names)]) + 4) & ~3
    # The table follows the pool in 1.8.0; scan there first, then all of rodata.
    for scan in (range(end, end + 0x100, 4), range(ro0 & ~3, ro0 + rol - 4, 4)):
        for off in scan:
            v = struct.unpack_from("<I", img, off)[0]
            if lo + v not in names:
                continue
            k = _sorted_run(img, off, lo, names)
            if k > best[2]:
                best = (off, lo, k)
        if best[2] > 100:
            break
    offsets, pool, count = best
    if count < 100:
        raise ValueError("sdk: no sorted NVN proc name table")
    rel = {o: a for o, t, _, a in nso.relocs if t == R_AARCH64_RELATIVE}
    t0, tl = nso.text
    slots = sorted(o for o, a in rel.items() if t0 <= a < t0 + tl)
    runs, s, p = [], slots[0], slots[0]
    for o in slots[1:]:
        if o != p + 8:
            runs.append((s, p + 8))
            s = o
        p = o
    runs.append((s, p + 8))
    runs = [(s, e) for s, e in runs if (e - s) // 8 >= count]
    refs = set().union(*(_code_refs(nso, s, e) for s, e in runs)) if runs else set()
    cands = []
    for s, e in runs:
        for t in sorted(r for r in refs if s <= r and r + 8 * count <= e):
            entries = [rel[t + 8 * i] for i in range(count)]
            if not any(_is_trampoline(img, a) for a in entries[:16]):
                cands.append(t)
    if len(cands) != 1:
        raise ValueError(f"sdk: {len(cands)} candidate NVN proc pointer tables: {[hex(c) for c in cands]}")
    return pool, offsets, count - 1, cands[0]


def tables(nso: Nso, search: bool = False) -> tuple[int, int, int, int]:
    if not search and nso.build_id in TABLES:
        return TABLES[nso.build_id]
    return locate(nso)


def procs(nso: Nso, search: bool = False) -> dict[str, int]:
    """'nvnName' -> offset of the function in the sdk module."""
    pool, offsets, last, pointers = tables(nso, search)
    img = bytes(nso.image)
    relocated = {off: add for off, typ, _, add in nso.relocs if typ == R_AARCH64_RELATIVE}
    out = {}
    for i in range(last + 1):
        name_at = pool + struct.unpack_from("<I", img, offsets + 4 * i)[0]
        name = img[name_at : img.index(0, name_at)].decode()
        out["nvn" + name] = relocated[pointers + 8 * i]
    return out


def main() -> None:
    search = "--locate" in sys.argv[1:]
    sdk = Nso(str(Path(os.environ.get("MKW_GAME", "game")) / "exefs/sdk"))
    table = procs(sdk, search)
    for name, off in sorted(table.items(), key=lambda t: t[1]):
        print(f"sdk+{off:#x} {name}")
    pool, offsets, last, pointers = tables(sdk, search)
    known = "known" if sdk.build_id in TABLES and not search else "located"
    print(
        f"{len(table)} procs; sdk {sdk.build_id[:16]} ({known}): pool {pool:#x}, offsets {offsets:#x}, "
        f"last {last:#x}, pointers {pointers:#x}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
