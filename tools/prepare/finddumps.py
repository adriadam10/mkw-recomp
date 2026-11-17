"""Find the player's copies of Mario Kart World in the usual places, for the start screen's setup: .nup and .nx
packages and loose exefs/ + romfs/ folders, each with its version when it can be read without unpacking.

Why: the setup's step 1 asks for the copy through the system's file dialogs (SDL_ShowOpenFileDialog), which need a
desktop's portal. In Steam's Gaming Mode (gamescope, on a Steam Deck or Bazzite) there is none, so a player with only a
controller could not pick the copy at all (issue #486 was a Deck set up in desktop mode). Listed here, each copy found
is one button on the setup page.

What counts: a .nup whose newest title is Mario Kart World (its table of contents: title id and version, as nup.py
reads it), a .nx package (NXAR; its version is read by the setup), and a folder with exefs/main of a build
recomp/versions.py knows beside a romfs/ folder. Where: the home folder's usual places (Downloads, Desktop,
Documents, Games, EmuDeck's Emulation), Steam's Proton prefixes (compatdata/*/pfx/drive_c, where #486's copy was),
removable media (/run/media, /media, /mnt: SteamOS mounts the SD card there) and on Windows the profile's folders and
each drive's root. A few levels deep, hidden folders skipped, within a time budget: a slow card cannot hold the start
screen's list back for long (launcher_finddumps.cpp reads the lines when the search ends; until then step 1 says it
is looking).

Output: one line per copy, "dump<TAB>nup|nx|folder<TAB><version or ?><TAB><path>", then "done <count>". The installation
folder itself (--game) is left out.

Usage: python -I finddumps.py [--game DIR] [--seconds 4] [ROOT...]   (ROOTs replace the usual places: tests)
"""

import argparse
import os
import string
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "recomp"))
from versions import VERSIONS  # noqa: E402  (recomp/ on the path first)

TITLE = 0x0400C3F00006E000  # Mario Kart World (nup.py pack --title)
NUP_VERSIONS = {0: "1.0.0", 917504: "1.8.0"}  # a .nup table's title version (nup.py pack --version)
BUILDS = {v.main_build_id: v.name for v in VERSIONS}
DEPTH = 4  # folders below a root: /run/media/deck/SD/Games/MKW.nup is 3
MAX_FOUND = 16
SKIP = {"node_modules", "proc", "sys", "dev", "Windows", "$Recycle.Bin", "System Volume Information", "shadercache"}


def nup_version(path: Path) -> str | None:
    """The version of a Mario Kart World .nup from its newest title, "?" for one not listed, None for another title or
    not a package (the layout of tools/prepare/nup.py Nup)."""
    try:
        with open(path, "rb") as f:
            head = f.read(12)
            if len(head) < 12 or head[:4] != b"NUP\x01":
                return None
            toc = struct.unpack_from("<I", head, 8)[0]
            f.seek(toc)
            th = f.read(0x30)
            if len(th) < 0x30 or th[:4] != b"TOC\x01":
                return None
            nt = struct.unpack_from("<I", th, 4)[0]
            if not 0 < nt < 64:
                return None
            f.seek(toc + 0x30 + (nt - 1) * 0x40)
            rec = f.read(0x18)
    except OSError:
        return None
    if len(rec) < 0x18:
        return None
    tid, _, _, _, version = struct.unpack_from("<QIIII", rec)
    if tid != TITLE:
        return None
    return NUP_VERSIONS.get(version, "?")


def is_nx(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"NXAR"
    except OSError:
        return False


def folder_version(path: Path) -> str | None:
    """The version of a loose copy (exefs/main of a known build beside romfs/), None otherwise."""
    try:
        if not (path / "romfs").is_dir():
            return None
        with open(path / "exefs" / "main", "rb") as f:
            head = f.read(0x48)
    except OSError:
        return None
    if len(head) < 0x48 or head[:4] != b"NSO0":
        return None
    return BUILDS.get(head[0x40:0x48].hex())


def usual_roots() -> list[Path]:
    home = Path.home()
    roots = [home / d for d in ("Downloads", "Desktop", "Documents", "Games", "Emulation", "Descargas", "Escritorio")]
    if os.name == "nt":
        roots += [Path(f"{d}:\\") for d in string.ascii_uppercase if Path(f"{d}:\\").exists()]
    else:
        for steam in (home / ".local/share/Steam", home / ".steam/steam",
                      home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"):  # fmt: skip
            compat = steam / "steamapps/compatdata"
            if compat.is_dir():
                roots += sorted(compat.glob("*/pfx/drive_c"))
        for media in ("/run/media", "/media", "/mnt"):
            if os.path.isdir(media):
                roots.append(Path(media))
        roots.append(home)  # last: what the others did not cover, a few levels deep
    seen, out = set(), []
    for r in roots:
        key = os.path.normcase(str(r))
        if key not in seen and r.is_dir():
            seen.add(key)
            out.append(r)
    return out


def search(roots: list[Path], game: Path | None, seconds: float, emit) -> int:
    end = time.monotonic() + seconds
    found, visited = 0, set()
    skip = os.path.normcase(str(game.resolve())) if game else None
    for root in roots:
        stack = [(root, 0)]
        while stack:
            if time.monotonic() > end or found >= MAX_FOUND:
                return found
            d, depth = stack.pop()
            try:
                real = os.path.normcase(os.path.realpath(d))
            except OSError:
                continue
            if real in visited or real == skip:
                continue
            visited.add(real)
            version = folder_version(d)
            if version:
                emit("folder", version, d)
                found += 1
                continue  # a copy's own folders hold no other copy
            try:
                entries = list(os.scandir(d))
            except OSError:
                continue
            for e in entries:
                name = e.name
                try:
                    if e.is_file(follow_symlinks=False):
                        low = name.lower()
                        if low.endswith(".nup"):
                            v = nup_version(Path(e.path))
                            if v:
                                emit("nup", v, Path(e.path))
                                found += 1
                        elif low.endswith(".nx") and is_nx(Path(e.path)):
                            emit("nx", "?", Path(e.path))
                            found += 1
                    elif e.is_dir(follow_symlinks=False) and depth < DEPTH:
                        if not name.startswith(".") and name not in SKIP:
                            stack.append((Path(e.path), depth + 1))
                except OSError:
                    continue
    return found


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", type=Path, help="the installation folder, left out")
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("roots", nargs="*", type=Path)
    a = ap.parse_args()

    def emit(kind: str, version: str, path: Path) -> None:
        print(f"dump\t{kind}\t{version}\t{path}", flush=True)

    n = search(a.roots or usual_roots(), a.game, a.seconds, emit)
    print(f"done {n}", flush=True)


if __name__ == "__main__":
    main()
