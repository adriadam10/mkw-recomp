"""prepare.py's copies of the player's game into the game folder: loose files COPY_JOBS at a time, resumable, and the
files the copied version does not have removed.

Why: its own module so prepare.py keeps the steps; imported by tools/prepare/prepare.py, nothing to run by hand.
"""

import os
import shutil
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

from machine import Failed, emit

COPY_JOBS = 6  # files copied at once: NTFS creates ~170 small files a second from one stream, ~460 from six (Windows test PC, 08/11/2026)


def same_contents(s: Path, t: Path) -> bool:
    """Check equal-sized files when the destination can contain bytes from another version."""
    if s.samefile(t):
        return True
    with s.open("rb") as source, t.open("rb") as target:
        while block := source.read(1 << 20):
            if block != target.read(len(block)):
                return False
        return not target.read(1)


def copy_one(s: Path, t: Path, *, verify_existing: bool = False) -> None:
    """Keep a complete copy, or write through .part so an interrupted copy is never taken."""
    if t.is_file() and t.stat().st_size == s.stat().st_size and (not verify_existing or same_contents(s, t)):
        return
    part = t.with_name(t.name + ".part")
    shutil.copyfile(s, part)
    part.replace(t)


def own_dir(d: Path, src: Path) -> bool:
    """d a folder of the game folder's own, not a link to another (the player's copy, a developer's game/romfs), nor
    one holding src (a package kept in game/romfs, the game folder as its own source): only those lose files."""
    if not d.is_dir() or d.is_symlink() or os.path.isjunction(d):
        return False
    return not src.resolve().is_relative_to(d.resolve())


def remove_stale(dst: Path, src: Path, kept: set[str], version: str) -> None:
    """dst's files that kept (paths under dst, with /) does not have removed, then the folders that left empty: a copy
    over a folder set up from another version leaves that version's files behind (1.0.0's general_format.Product_100
    among 1.8.0's Product_180), and tools that look for the game's own files then find two: the resolution overlay
    built from the stale one, mods checked against it (#261)."""
    if not own_dir(dst, src):
        return
    removed, dirs = 0, []
    for d, subdirs, files in os.walk(dst, topdown=False):
        dirs += [Path(d, s) for s in subdirs]
        for f in files:
            p = Path(d, f)
            if p.relative_to(dst).as_posix() not in kept:
                p.unlink()
                removed += 1
    for d in dirs:
        try:
            d.rmdir()  # only when the pruning emptied it
        except OSError:
            pass
    if removed:
        print(f"copy: removed {removed} stale files that {version} does not have from {dst}", file=sys.stderr)


def clear_dir(d: Path, src: Path) -> None:
    """d emptied before a copy from src that writes every file anyway (mkw --unpack of a .nx), so no other version's
    stay."""
    if own_dir(d, src):
        shutil.rmtree(d)
        print(f"copy: {d} emptied before unpacking {src}", file=sys.stderr)


def copy_tree(src: Path, dst: Path, total: int, version: str = "", *, verify_existing: bool = False) -> None:
    """src into dst with progress, COPY_JOBS files at a time: the RomFS's 205 660 files one by one were most of a
    copy's time on Windows, where creating a file costs more than its bytes. With version, then the files src does
    not have are removed (remove_stale), so the folder is src's."""
    done = 0
    kept: set[str] = set()
    with ThreadPoolExecutor(COPY_JOBS) as pool:
        pending = []
        for d, _, files in os.walk(src):
            out = dst / Path(d).relative_to(src)
            out.mkdir(parents=True, exist_ok=True)
            pending += [pool.submit(copy_one, Path(d, f), out / f, verify_existing=verify_existing) for f in files]
            kept.update(Path(d, f).relative_to(src).as_posix() for f in files)
        for fut in as_completed(pending):
            fut.result()
            done += 1
            if done % 500 == 0 or done == total:
                emit("progress", done, total)
    if version:
        remove_stale(dst, src, kept, version)


# What a setup writes besides a copied RomFS, measured on a 1.8.0 build (17/11/2026): the generated C 0.95 GiB, its
# objects 0.65, the SPIR-V 0.98, the game library 0.24 and the shader stages 0.16.
GIB = 1 << 30
BUILD_BYTES = 3 * GIB
ROMFS_BYTES = 22 * GIB  # a .nx package's RomFS, whose size it does not say: 1.8.0's is 21.9 GiB in 206 202 files


def romfs_bytes(src: Path) -> int:
    """What a copy of src's RomFS takes: a loose folder's own size, a .nup's file sizes, else (a .nx) ROMFS_BYTES."""
    if src.is_dir():
        return tree_bytes(src)
    from nup import Nup, is_nup  # noqa: PLC0415 - only when a package is copied

    if is_nup(src):
        return sum(e.size for e in Nup(src).files("romfs").values())
    from gamesource import is_nx  # noqa: PLC0415

    return ROMFS_BYTES if is_nx(src) else 0


def tree_bytes(d: Path) -> int:
    """The bytes already in d: what a copy cut short left (copy_one keeps a file of the right size)."""
    total = 0
    for top, _, files in os.walk(d):
        for f in files:
            try:
                total += os.stat(os.path.join(top, f)).st_size
            except OSError:
                pass
    return total


def check_space(game: Path, need: int) -> None:
    """Fails before the setup writes anything when the game folder's drive has less than need free: a disk filled
    half-way through the RomFS copy failed with the system's "No space left on device" after up to 20 GiB and left the
    cut copy taking that room (a Steam Deck of 64 GB, a full SD card). The start screen says it in the player's
    language, and that keeping the RomFS where it is needs no copy."""
    at = game
    while not at.exists() and at.parent != at:
        at = at.parent
    free = shutil.disk_usage(at).free
    if need and free < need:
        detail = f"{need / GIB:.1f} GB, {free / GIB:.1f} GB free in {at}"
        raise Failed(f"not enough free space for the setup: it needs {detail}", "space", detail)
