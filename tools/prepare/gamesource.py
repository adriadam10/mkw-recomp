"""The check step of prepare.py: the player's game copy (loose exefs/ and romfs/, a .nup or a .nx package) found, its
version read from exefs/main's build id, and its RomFS checked against that version (file count and game data
version), plus the unpacking of packages through the runtime.

Why apart: prepare.py holds the order of the steps and their keys; this is everything about the copy itself. Not run
on its own.
"""

import os
import subprocess
import sys
from pathlib import Path

from machine import Failed, emit
from nup import Nup, is_nup

# (prepare.py put recomp/ on the path, through build.py)
from versions import ROMFS_PRODUCT, Version, build_id_of, detect_id, romfs_mismatch

ROMFS_MARGIN = 0.95  # a few short still passes: some dumping tools skip empty files


def is_nx(path: Path) -> bool:
    """A .nx package (NXFormat): read through the runtime (mkw --unpack, runtime/sdk/fs/nx.cpp), which has its reader."""
    try:
        with path.open("rb") as f:
            return f.read(4) == b"NXAR"
    except OSError:
        return False


def unpack(runtime: Path, pkg: Path, dest: Path | str, *prefixes: str) -> str:
    """Files of a packed dump (.nup or .nx) under dest as loose files, by the runtime: only those under the prefixes
    ("exefs", "romfs/Mals"). Returns its "unpack: N files, B bytes" line (the log lines come first); dest "-" counts only."""
    exe = runtime / ("mkw.exe" if os.name == "nt" else "mkw")
    p = subprocess.Popen([str(exe), "--unpack", str(pkg), str(dest), *prefixes], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, encoding="utf-8", errors="replace")  # fmt: skip
    head, tail = "", []
    for line in p.stdout:
        if line.startswith("progress "):
            emit(*line.split())
        else:
            head = head or (line.strip() if line.startswith("unpack: ") else "")
            tail = (tail + [line.strip()])[-5:]
            sys.stderr.write(line)
    if p.wait():
        raise Failed(f"{pkg} could not be unpacked: {' / '.join(tail)}", "source", str(pkg))
    return head


def loose_romfs(runtime: Path, romfs: Path, work: Path, *dirs: str) -> Path:
    """romfs as a folder with at least dirs in it: itself when loose, else those dirs unpacked from the package."""
    if not romfs.is_file():
        return romfs
    unpack(runtime, romfs, work / "pack", *(f"romfs/{d}" for d in dirs))
    return work / "pack" / "romfs"


def find_game(src: Path) -> tuple[Path, Path]:
    """src a .nup or .nx package (both are then its path), else exefs/ and romfs/ in src or up to two levels below it."""
    if src.is_file():
        if is_nup(src) or is_nx(src):
            return src, src
        raise Failed(f"{src} is not a .nup or .nx package", "nogame", str(src))
    for depth in range(3):
        for d in [src, *src.glob("/".join(["*"] * depth))] if depth else [src]:
            # romfs/ loose, or a .nup (or a link to one) next to a loose exefs/: a packed copy with its exefs for tools
            if (d / "exefs/main").is_file() and ((d / "romfs").is_dir() or is_nup(d / "romfs")):
                return d / "exefs", d / "romfs"
    raise Failed(f"no exefs/main and romfs/ in {src}", "nogame", str(src))


def count_files(top: Path, enough: int) -> int:
    n = 0
    for _, _, files in os.walk(top):
        n += len(files)
        if n >= enough:
            break
    return n


def check(src: Path, game: Path, runtime: Path | None = None) -> tuple[Path, Path, Version]:
    runtime = runtime or game
    exefs, romfs = find_game(src)
    if is_nx(exefs):  # a .nx package: its exefs/main through the runtime
        unpack(runtime, exefs, game / "setup-work/pack", "exefs/main")
        main = game / "setup-work/pack/exefs/main"
        head, where = main.read_bytes()[:0x48], f"{exefs}::exefs/main"
    elif exefs.is_file():  # a .nup package
        nup = Nup(exefs)
        if "main" not in nup.files("exefs"):
            raise Failed(f"{exefs} has no exefs/main", "nogame", str(exefs))
        head, where = nup.read(nup.files("exefs")["main"])[:0x48], f"{exefs}::exefs/main"
    else:
        head, where = (exefs / "main").read_bytes()[:0x48], str(exefs / "main")
    try:
        got = build_id_of(head, where)
    except ValueError:
        raise Failed(f"{where} is not an NSO: decrypted exefs needed", "encrypted", where) from None
    try:
        v = detect_id(got)
    except ValueError as e:
        raise Failed(str(e), "version", got) from None
    need = int(v.romfs_files * ROMFS_MARGIN)
    if is_nx(romfs):
        n = int(unpack(runtime, romfs, "-", "romfs").split()[1])
    else:
        n = len(Nup(romfs).files("romfs")) if romfs.is_file() else count_files(romfs, need)
    if n < need:
        raise Failed(f"the RomFS has {n} files, {v.name} has {v.romfs_files}: an incomplete copy", "romfs", str(n))
    if not is_nx(romfs):
        res = Path(ROMFS_PRODUCT).parent
        names = (
            Nup(romfs).files("romfs")
            if romfs.is_file()
            else (f"{res.as_posix()}/{p.name}" for p in (romfs / res).glob("*"))
        )
        if why := romfs_mismatch(v, names):
            raise Failed(f"the RomFS and exefs/main are from different versions ({why})", "mismatch", why)
    emit("version", v.name)
    return exefs, romfs, v
