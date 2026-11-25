"""Optional third-party libraries (DLSS, FSR 3.1, RenHDrum) fetched from their official sources when the player turns
their option on, each checked against the size and sha256 pinned in libs.tsv and put beside the program.

Why: players had to find these files and copy them by hand, and kept asking how (Discord, #788, #803). None of them is
in our package: NVIDIA's DLSS licence forbids distributing its files on their own, so each player's launcher downloads
them from NVIDIA's, AMD's or RenHDrum's own release. A file already there is kept, whatever its version: a player may
have put a newer DLSS there on purpose.

Output: one event per line on stdout, as update.py's:
  missing  "need <feature> <file> <bytes>" for each absent file, then "done"
  fetch    "progress <done> <total>" (bytes of all the files fetched), "ready <file>" each, then "done"
  any      "error <command> <message>"
Exit codes: 0 done, 1 failed.

Usage: python -I libs.py missing --features dlss,fsr3 --dir DIR [--os linux|windows]
       python -I libs.py fetch --features dlss,fsr3 --dir DIR [--os linux|windows]
Features: dlss, dlssg, fsr3, fsr3fg, renhdrum (the rows of libs.tsv for that system; none is not an error).
"""

import argparse
import csv
import hashlib
import http.client
import platform
import sys
import time
import urllib.request
from contextlib import suppress
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from update import FETCH_TIMEOUT, emit, ssl_context  # noqa: E402

TABLE = Path(__file__).resolve().with_name("libs.tsv")


class Lib(NamedTuple):
    feature: str
    os: str
    file: str
    size: int
    sha256: str
    url: str


def table() -> list[Lib]:
    with TABLE.open(encoding="utf-8", newline="") as f:
        return [
            Lib(r["feature"], r["os"], r["file"], int(r["bytes"]), r["sha256"], r["url"])
            for r in csv.DictReader(f, delimiter="\t")
        ]


def wanted(features: str, os_name: str) -> list[Lib]:
    names = {x for x in features.split(",") if x}
    known = {lib.feature for lib in table()}
    if unknown := names - known:
        raise ValueError(f"unknown feature {', '.join(sorted(unknown))} (known: {', '.join(sorted(known))})")
    return [lib for lib in table() if lib.feature in names and lib.os == os_name]


def absent(args: argparse.Namespace) -> list[Lib]:
    return [lib for lib in wanted(args.features, args.os) if not (args.dir / lib.file).is_file()]


def missing(args: argparse.Namespace) -> None:
    for lib in absent(args):
        emit("need", lib.feature, lib.file, lib.size)
    emit("done")


def download(lib: Lib, dest: Path, done: int, total: int, retries: int = 3) -> int:
    """lib into dest through a .part file renamed only once its size and sha256 match; returns done + its bytes."""
    part = dest / (lib.file + ".part")
    step = max(1, total // 200)
    req = urllib.request.Request(lib.url, headers={"User-Agent": "mkw-recomp-launcher"})
    for attempt in range(retries):
        h, got = hashlib.sha256(), 0
        try:
            with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT, context=ssl_context()) as r, part.open("wb") as f:
                while chunk := r.read(1 << 20):
                    f.write(chunk)
                    h.update(chunk)
                    if (done + got) // step != (done + got + len(chunk)) // step:
                        emit("progress", done + got + len(chunk), total)
                    got += len(chunk)
                    if got > lib.size:
                        break
            break
        except (OSError, http.client.HTTPException):
            with suppress(OSError):
                part.unlink(missing_ok=True)
            if attempt + 1 == retries:
                raise
            time.sleep(1)
    if got != lib.size or h.hexdigest() != lib.sha256:
        part.unlink()
        raise ValueError(f"{lib.file} came damaged ({got} of {lib.size} bytes or another sha256): try again")
    part.replace(dest / lib.file)
    emit("ready", lib.file)
    return done + got


def fetch(args: argparse.Namespace) -> None:
    libs = absent(args)
    args.dir.mkdir(parents=True, exist_ok=True)
    total, done = sum(lib.size for lib in libs), 0
    for lib in libs:
        done = download(lib, args.dir, done, total)
    emit("done")


def this_os() -> str:
    return "windows" if platform.system() == "Windows" else "linux"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("missing", "fetch"):
        p = sub.add_parser(name)
        p.add_argument("--features", required=True)
        p.add_argument("--dir", type=Path, required=True)
        p.add_argument("--os", choices=("linux", "windows"), default=this_os())
    args = ap.parse_args()
    try:
        {"missing": missing, "fetch": fetch}[args.cmd](args)
    except (OSError, ValueError, http.client.HTTPException) as e:
        emit("error", args.cmd, str(e).replace("\n", " "))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
