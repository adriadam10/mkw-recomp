"""Back up a player's saves: their profile folder as one zip in the game folder's backups/, named after the player and
the time. The start screen's Tools page runs it (ui/launcher/launcher_tools.cpp) so a save survives a broken update or a
deleted folder; the zip opens with any archiver and goes back by unpacking it over the profile folder.

Prints one line for the start screen: "done <zip>", or "error <code> <detail>" (code: noprofile, io).

Usage: python tools/prepare/saves.py backup --profile <player folder> --dest <game>/backups --name <player name>
"""

import argparse
import re
import stat
import sys
import time
import zipfile
from pathlib import Path


def raise_scan_error(error: OSError) -> None:
    raise error


def backup(profile: Path, dest: Path, name: str) -> str:
    if not profile.is_dir():
        raise FileNotFoundError(f"noprofile {profile}")
    dest.mkdir(parents=True, exist_ok=True)
    safe = re.sub(r"[^\w.-]+", "_", name).strip("._") or "player"
    stem = f"{safe}-{time.strftime('%Y%m%d-%H%M%S')}"
    number = 0
    while True:
        out = dest / f"{stem}{f'-{number}' if number else ''}.zip"
        number += 1
        if out.exists():
            continue
        tmp = out.with_suffix(".part")
        try:
            stream = tmp.open("xb")  # reserve the name across processes; stale partials stay untouched
        except FileExistsError:
            continue
        # Another backup may have published after our exists() but before we reserved its old partial name.
        if out.exists():
            stream.close()
            tmp.unlink()
            continue
        try:
            with stream, zipfile.ZipFile(stream, "w", zipfile.ZIP_DEFLATED) as z:
                # Skipping an unreadable directory would publish a ZIP with missing saves.
                for folder, dirs, files in profile.walk(on_error=raise_scan_error):
                    for entry in dirs + files:
                        path = folder / entry
                        if (path.is_symlink() or path.is_junction()) and path.is_dir():
                            raise OSError(f"cannot back up linked directory: {path}")
                    dirs.sort()
                    for filename in sorted(files):
                        path = folder / filename
                        try:
                            mode = path.stat().st_mode
                        except FileNotFoundError:
                            if path.is_symlink():
                                continue  # a dangling link has no file contents to back up
                            raise
                        if stat.S_ISREG(mode):
                            z.write(path, path.relative_to(profile))
            tmp.replace(out)  # a backup is whole or absent, never half written
        except BaseException:
            tmp.unlink(missing_ok=True)
            raise
        return str(out)


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")  # the launcher reads this pipe as UTF-8
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("backup")
    b.add_argument("--profile", type=Path, required=True)
    b.add_argument("--dest", type=Path, required=True)
    b.add_argument("--name", default="player")
    args = ap.parse_args()
    try:
        print("done", backup(args.profile, args.dest, args.name), flush=True)
    except FileNotFoundError as e:
        print("error", e.args[0] if str(e.args[0]).startswith("noprofile") else f"io {e}", flush=True)
        sys.exit(1)
    except OSError as e:
        print("error io", e, flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
