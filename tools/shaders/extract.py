"""Extract every GPU program of the game's shader archives into build/shaders/, one file per distinct stage.

Why: the game's shaders are a closed set (BNSH inside .bfsha, and agl .sharcb packages), so they are translated ahead
of time, never discovered in play. Layout as in NVN1 (BNSH variations -> program -> stage records at +0x08 vs, +0x10
tcs, +0x18 tes, +0x20 gs, +0x28 ps, +0x30 cs); NVN2 changed the control section (0xd8 bytes, same 0x98761234 magic)
and the code: graphics stages start with a 0x80-byte program header (SPH), compute stages are bare SASS SM86.
BNSH walk adapted from totk-pc-recomp tools/shader_census.py @ c3b9a4e (MIT).

Usage: uv run tools/shaders/extract.py [--romfs DIR|FILE.nup] [--out build/shaders] [--everything]
Writes <sha1>.ctl and <sha1>.code per stage and index.csv (sha1, stage, code bytes, source file, program index),
<sha1>.c1 with the program's own constants (bank c[1]) when it has any: a BRX's jump table lives there, and pairs.csv
(vs sha1, ps sha1): every vertex and fragment stage one program can draw together (an agl program: each of its vertex
variations with each fragment one). index.csv names each stage once, by the first program holding it, so the pairs
come from pairs.csv: pairing by its program column found 962 of the 3,119 pairs a run drew.
"""

import argparse
import csv
import functools
import hashlib
import itertools
import os
import struct
import sys
from collections import Counter
from compression import zstd
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROMFS = Path(os.environ.get("MKW_GAME", "game")) / "romfs"
CONTROL_MAGIC = b"\x34\x12\x76\x98"
CONTROL_SIZE = 0xD8
SPH_SIZE = 0x80
NOP_LOW16 = 0x7918  # low 16 bits of the SM86 NOP encoding (padding)
CODE_SIZE_AT = 0x44  # u32 in the control section: code bytes, SPH included
C1_AT = 0x3C  # u32 size and u32 offset in the code blob of the program's constants, bank c[1] (vk_exec_bind.c ubo_info)


def constants(ctl: bytes, blob: bytes) -> bytes:
    """The program's c[1] bytes from its untrimmed code blob; b'' without them."""
    size, off = struct.unpack_from("<2I", ctl, C1_AT)
    return blob[off : off + size] if size and off + size <= len(blob) else b""
SLOT_STAGES = {0x08: "vs", 0x10: "tcs", 0x18: "tes", 0x20: "gs", 0x28: "ps", 0x30: "cs"}
# Type 3 is TES on Maxwell; MKW has no tessellation and its 12 type-3 programs match the 12 task/mesh programs
# eden-nx2 could not lower (NX2_NOTES.md row 88), so it is named apart until the SPH fields are read.
SPH_STAGES = {1: "vs", 2: "tcs", 3: "tes?", 4: "gs", 5: "ps"}


def sph_stage(code: bytes) -> str | None:
    """Stage named by a Maxwell-style SPH (type 1 VTG / 2 PS, version 4), or None for bare code."""
    w = struct.unpack_from("<I", code)[0]
    if w & 0x1F not in (1, 2) or (w >> 5) & 0x1F != 4:
        return None
    return SPH_STAGES.get((w >> 10) & 0xF)


def trim(code: bytes, stage: str) -> bytes:
    """Cut a program where it ends: at its first all-zero 128-bit word (no SASS instruction encodes as zero), or at a
    program header right after NOP padding (agl packages lay programs back to back, so the next one's SPH follows)."""
    start = 0 if stage == "cs" else SPH_SIZE
    for at in range(start, len(code), 16):
        word = code[at : at + 16]
        if word == bytes(16):
            return code[:at]
        after_nop = at > start and int.from_bytes(code[at - 16 : at - 14], "little") == NOP_LOW16
        if after_nop and sph_stage(word):
            return code[:at]
    return code


def bnsh_stages(bnsh: bytes):
    """(program index, slot stage, control, code) for every stage of one BNSH."""
    if bnsh[:4] != b"BNSH" or bnsh[12:14] != b"\xff\xfe" or bnsh[0x60:0x64] != b"grsc":
        raise ValueError("unsupported BNSH header")
    bnsh = bnsh[: struct.unpack_from("<I", bnsh, 28)[0]]
    count, variations = struct.unpack_from("<IQ", bnsh, 124)
    for i in range(count):
        program = struct.unpack_from("<Q", bnsh, variations + i * 64 + 16)[0]
        for rel, stage in SLOT_STAGES.items():
            rec = struct.unpack_from("<Q", bnsh, program + rel)[0]
            if not rec:
                continue
            ctl_off, code_off, code_size, ctl_size = struct.unpack_from("<QQII", bnsh, rec + 8)
            ctl = bnsh[ctl_off : ctl_off + ctl_size]
            if ctl[:4] != CONTROL_MAGIC:
                raise ValueError(f"program {i} {stage}: control without magic")
            yield i, stage, ctl, bnsh[code_off : code_off + code_size]


def bahs_variations(data: bytes, entries: set[int]) -> dict[int, str]:
    """Entry offset -> "program.variation" of an agl package's vertex and fragment binaries; {} when the arrays do not
    read back. The table area holds a binary array (u32 byte size, u32 count, then per binary: u32 record size, u32 kind
    0 vertex / 1 fragment / 3 compute, two u32, the 16-byte entry sharcb_stages reads) right before a program array
    (u32 byte size, u32 count, then per program: u32 record size, u32 name length, u32 stage mask, u32 index of its
    first binary, name, macros). A program's binaries run up to the next program's first; its n-th vertex and n-th
    fragment binary are one variation. Layout from nxnative dm/gfx/tools/shader_split.py bahs_variations @ e4b0d9b (MIT).
    Without it the census paired none of the 136 program pairs of agl packages a run drew (17/11/2026)."""
    if not entries:
        return {}
    arr = min(entries) - 0x18
    if arr < 0x20:
        return {}
    size, count = struct.unpack_from("<II", data, arr)
    at, recs = arr + 8, []
    for _ in range(count):
        if at + 0x20 > arr + size:
            return {}
        rec_size, kind = struct.unpack_from("<II", data, at)
        if rec_size < 0x20:
            return {}
        recs.append((at + 0x10, kind))
        at += rec_size
    if at != arr + size:
        return {}
    psize, pcount = struct.unpack_from("<II", data, at)
    q, bases = at + 8, []
    for _ in range(pcount):
        if q + 16 > at + psize:
            return {}
        rec_size, _name, _mask, base = struct.unpack_from("<4I", data, q)
        if rec_size < 16 or base > count or (bases and base < bases[-1]):
            return {}
        bases.append(base)
        q += rec_size
    if not pcount or q != at + psize:
        return {}
    bases.append(count)
    out = {}
    for p in range(pcount):
        seen = [0, 0]
        for entry, kind in recs[bases[p] : bases[p + 1]]:
            if kind > 1:
                continue
            out[entry] = f"{p}.{seen[kind]}"
            seen[kind] += 1
    return out


def sharcb_stages(data: bytes):
    """Stages of an agl package (BAHS): records <u64 code offset in pool, u32 control size, i32 control offset>. The
    program index is "program.variation" (bahs_variations) when the package's arrays read back, else a running number."""
    start, pool_size, pool = struct.unpack_from("<3I", data, 20)
    at, found, entry_of = start, {}, {}
    size_word = struct.pack("<I", CONTROL_SIZE)
    while (at := data.find(size_word, at, pool)) >= 0:
        rec, at = at - 8, at + 4
        if rec < start or rec % 4:
            continue
        code_off = struct.unpack_from("<Q", data, rec)[0]
        ctl_at = rec + struct.unpack_from("<i", data, rec + 12)[0]
        if not start <= ctl_at <= pool - CONTROL_SIZE or code_off >= pool_size:
            continue
        ctl = data[ctl_at : ctl_at + CONTROL_SIZE]
        if ctl[:4] == CONTROL_MAGIC and code_off not in found:
            found[code_off], entry_of[code_off] = ctl, rec
    # Records carry no code size; the control section does at +0x44 (equal to the BNSH record's size in all 3,238
    # programs of the .bfsha files, SPH included).
    variation = bahs_variations(data, set(entry_of.values()))
    for n, (code_off, ctl) in enumerate(sorted(found.items())):
        size = struct.unpack_from("<I", ctl, CODE_SIZE_AT)[0]
        if code_off + size > pool_size:
            raise ValueError(f"agl program {n}: code past the pool")
        yield variation.get(entry_of[code_off], n), None, ctl, data[pool + code_off : pool + code_off + size]


def ndsr_stages(data: bytes):
    """Stages of the ndsr upscaler's package (Lib/agl/agl_resource...sarc/agl_ndsr.pck, magic NDSR): 16-byte records
    from +0x10 <u32 control offset, u32 kind, u32 code offset, u32 code size << 4 | stage> before the first control
    (36 in 1.0.0: 3 vs, 3 ps, 30 cs). The controls are found by magic, their records by offset."""
    controls, at = [], 0
    while (at := data.find(CONTROL_MAGIC, at)) >= 0:
        controls.append(at)
        at += 4
    for n, ctl_at in enumerate(controls):
        rec = data.find(struct.pack("<I", ctl_at), 0x10, controls[0])
        if rec < 0 or rec % 4:
            continue
        code_off = struct.unpack_from("<I", data, rec + 8)[0]
        ctl = data[ctl_at : ctl_at + CONTROL_SIZE]
        yield n, None, ctl, data[code_off : code_off + struct.unpack_from("<I", ctl, CODE_SIZE_AT)[0]]


def sarc_members(data: bytes) -> dict[str, bytes]:
    header = struct.unpack_from("<H", data, 4)[0]
    data_start = struct.unpack_from("<I", data, 12)[0]
    count = struct.unpack_from("<H", data, header + 6)[0]
    names = header + 12 + count * 16 + 8
    out = {}
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from("<4I", data, header + 12 + i * 16)
        off = names + (attr & 0xFFFFFF) * 4
        out[data[off : data.index(0, off)].decode()] = data[data_start + lo : data_start + hi]
    return out


def containers(name: str, data: bytes, depth: int = 0) -> list[tuple[str, bytes]]:
    """Every BNSH (tagged FSHA, walked as one) and agl package in data, opening zstd frames and SARC members at any
    depth. All BNSH of a file count: DriveRefreshUber.Procedural...bfsha holds 4 (one per shading model) and only the
    first was read, which left 25 programs of run76 out of the census (31/10/2026)."""
    if data[:4] == b"\x28\xb5\x2f\xfd":
        try:
            data = zstd.decompress(data)
        except zstd.ZstdError:
            return []
    if data[:4] == b"SARC" and depth < 4:   # agl_resource...sarc, Drive...sarc.zs; members may be archives again
        try:
            members = sarc_members(data)
        except (struct.error, ValueError, UnicodeDecodeError):
            members = {}
        if members:
            return [c for m, member in members.items() for c in containers(f"{name}/{m}", member, depth + 1)]
    out, pos = [], 0
    while (pos := data.find(b"BNSH\0\0\0\0", pos)) >= 0:
        size = struct.unpack_from("<I", data, pos + 28)[0] if pos + 32 <= len(data) else 0
        if 0x60 < size <= len(data) - pos and data[pos + 0x60 : pos + 0x64] == b"grsc":
            out.append((f"{name}@{pos:#x}", b"FSHA" + data[pos : pos + size]))
        pos += 8
    if data[:4] in (b"BAHS", b"NDSR") or b"BAHS" in data[:16]:
        out.append((name, data))
    return out


@functools.cache  # parsed once per process: the scan's workers read many files of one package
def package(romfs: Path):
    """The .nup package (tools/prepare/nup.py) when --romfs names one, else None for a loose romfs/ folder."""
    if not romfs.is_file():
        return None
    sys.path.insert(0, str(ROOT / "tools/prepare"))
    from nup import Nup

    return Nup(romfs)


def archives(romfs: Path):
    """(name, bytes) of every BNSH container and agl package under romfs/Shader."""
    if nup := package(romfs):
        files = nup.files("romfs")
        for name in (n for n in files if n.startswith("Shader/") and n.count("/") == 1 and n.endswith(".zs")):
            yield from containers(name.split("/")[1], nup.read(files[name]))
        return
    for path in sorted((romfs / "Shader").glob("*.zs")):
        yield from containers(path.name, path.read_bytes())


def read_source(path: str) -> bytes:
    """A host file, or "<package>::<RomFS path>" for a file in a .nup package."""
    if "::" in path:
        nup_path, name = path.split("::", 1)
        nup = package(Path(nup_path))
        return nup.read(nup.files("romfs")[name])
    return Path(path).read_bytes()


def scan_one(path: str) -> list[tuple[str, bytes]]:
    """containers() of one RomFS file or NSO (NSO: its LZ4 segments, where main keeps two BNSH of its own,
    main+0x3328000 and main+0x3345000)."""
    data = read_source(path)
    if data[:4] == b"NSO0":
        sys.path.insert(0, str(ROOT / "recomp"))
        from nso import Nso

        return containers(path, bytes(Nso(path).image))
    return containers(path, data)


# Where 1.0.0 keeps every distinct stage outside romfs/Shader: a scan of every file (21.7 GiB) found none elsewhere
# (11/11/2026), and these hold ~0.4 GiB, so a player's cold disk reads 50 times less. --everything scans them all.
SHADER_DIRS = ("Effect", "Layout", "Lib")


def everywhere(romfs: Path, exefs: Path, everything: bool = False):
    """BNSH outside romfs/Shader (effects, layouts, the SDK), scanning the files in parallel."""
    import os
    from concurrent.futures import ProcessPoolExecutor

    if nup := package(romfs):
        names = nup.files("romfs")
        if not everything:
            names = [n for n in names if n.split("/", 1)[0] in SHADER_DIRS]
        files = [f"{romfs}::{n}" for n in names if not n.startswith("Shader/") and "/Shader/" not in n]
    else:
        tops = [romfs] if everything else [romfs / d for d in SHADER_DIRS]
        files = [str(p) for top in tops for p in top.rglob("*") if p.is_file() and "Shader" not in p.parts]
    files += [str(p) for p in sorted(exefs.glob("*")) if p.name in ("main", "sdk") or p.name.startswith("subsdk")]
    # Windows' ProcessPoolExecutor takes at most 61 workers: a dual Xeon with 80 threads stopped here (#222)
    workers = min(os.cpu_count() or 1, 61) if os.name == "nt" else os.cpu_count()
    with ProcessPoolExecutor(workers) as pool:
        for found in pool.map(scan_one, files, chunksize=64):
            yield from found


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--romfs", type=Path, default=DEFAULT_ROMFS)
    ap.add_argument("--out", type=Path, default=ROOT / "build/shaders")
    ap.add_argument("--shader-dir", action="store_true", help="only romfs/Shader (~10 s instead of ~2 min)")
    ap.add_argument("--everything", action="store_true", help=f"every RomFS file, not only {', '.join(SHADER_DIRS)}")
    ap.add_argument("--exefs", type=Path, default=Path(os.environ.get("MKW_GAME", "game")) / "exefs")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rows, seen, stages, pairs = [], set(), Counter(), set()
    sources = archives(args.romfs)
    if not args.shader_dir:
        sources = itertools.chain(sources, everywhere(args.romfs, args.exefs, args.everything))
    for name, data in sources:
        if data[:4] == b"FSHA":
            try:
                found = list(bnsh_stages(data[4:]))
            except ValueError, struct.error:
                continue
        elif data[:4] == b"BAHS":
            found = sharcb_stages(data)
        elif data[:4] == b"NDSR":
            found = ndsr_stages(data)
        else:
            continue
        program, last_vs = {}, None
        for index, slot_stage, ctl, code in found:
            stage = sph_stage(code) or slot_stage or "cs"
            c1 = constants(ctl, code)
            code = trim(code, stage)
            key = hashlib.sha1(ctl + code).hexdigest()
            if data[:4] == b"NDSR":  # a vertex record is followed by its pixel record (nxnative ndsr_shaders)
                last_vs = index if stage == "vs" else last_vs
                index = last_vs
            if stage in ("vs", "ps") and index is not None:
                # an agl program's variations pick vertex and fragment binaries apart (program 59 of
                # DrivePassShaderDefault draws vs variation 33 with ps 129): every vs of a program with every ps
                group = index.split(".")[0] if isinstance(index, str) else index
                program.setdefault(group, {"vs": set(), "ps": set()})[stage].add(key)
            if key in seen:
                continue
            seen.add(key)
            stages[stage] += 1
            # named by content: a file an earlier run left is already this program (the setup keeps the folder
            # between updates, tools/prepare/prepare.py clear_work)
            for ext, data_ in (("ctl", ctl), ("code", code), ("c1", c1)):
                p = args.out / f"{key}.{ext}"
                if data_ and not (p.is_file() and p.stat().st_size == len(data_)):
                    p.write_bytes(data_)
            rows.append((key, stage, len(code), name, index))
        pairs |= {(v, f) for p in program.values() for v in p["vs"] for f in p["ps"]}
    with (args.out / "index.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(("sha1", "stage", "code_bytes", "source", "program"))
        w.writerows(rows)
    with (args.out / "pairs.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(("vs", "ps"))
        w.writerows(sorted(pairs))
    counts = ", ".join(f"{s} {n}" for s, n in stages.most_common())
    print(f"{len(rows)} distinct stages ({len(pairs)} vs+ps pairs): {counts}")


if __name__ == "__main__":
    main()
