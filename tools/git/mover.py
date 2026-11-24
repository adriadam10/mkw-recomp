"""Move files of one folder into subfolders and rewrite every repo path that names them.

Why: splitting a folder by responsibility (runtime/gfx/vk had 66 loose files) moves dozens of files that docs, PLANs,
tools and comments cite as `runtime/gfx/vk/vk_memory.c`; tools/docs/check.py and the docs hook stop the commit while
one is left. Only full paths from the repo root are rewritten (bare names in comments stay true: the include path
covers every folder with a header, tools/build/build.py). The new subfolders' READMEs are written by hand.

Map file: one line per subfolder, `subfolder file file...` (files of FOLDER, by name).
Usage: uv run tools/git/mover.py FOLDER MAP            (dry run: what moves and which files would change)
       uv run tools/git/mover.py FOLDER MAP --apply    (git mv, then the rewrites)
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEXT = {".md", ".py", ".sh", ".tsv", ".c", ".h", ".cpp", ".inc", ".toml", ".txt", ".json", ""}


def read_map(path: Path, folder: Path) -> dict[str, str]:
    moves = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        sub, *names = line.split()
        for n in names:
            if not (folder / n).is_file():
                sys.exit(f"mover: {folder / n} does not exist")
            if n in moves:
                sys.exit(f"mover: {n} is mapped twice")
            moves[n] = sub
    return moves


def tracked_text() -> list[Path]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    files = [ROOT / p for p in out.decode().split("\0") if p]
    return [p for p in files if p.suffix in TEXT and p.is_file() and not p.is_symlink()]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("folder", type=Path)
    ap.add_argument("map", type=Path)
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()
    folder = (ROOT / a.folder).resolve()
    rel = folder.relative_to(ROOT).as_posix()
    moves = read_map(a.map, folder)
    loose = sorted(p.name for p in folder.iterdir() if p.is_file() and p.name != "README.md" and p.name not in moves)
    if loose:
        print(f"mover: left loose in {rel}: {' '.join(loose)}")
    # a path ends at a character that cannot continue a file name
    pat = re.compile(
        rf"(?<![\w/.-]){re.escape(rel)}/({'|'.join(map(re.escape, sorted(moves, key=len, reverse=True)))})(?![\w.-])"
    )
    changed = {}
    for p in tracked_text():
        try:
            with p.open(encoding="utf-8", newline="") as f:  # as written: UTF-8, line endings kept
                s = f.read()
        except UnicodeDecodeError:
            print(f"mover: not UTF-8, its paths are not rewritten: {p.relative_to(ROOT)}")
            continue
        t = pat.sub(lambda m: f"{rel}/{moves[m.group(1)]}/{m.group(1)}", s)
        if t != s:
            changed[p] = t
    print(f"{len(moves)} files into {len(set(moves.values()))} subfolders; {len(changed)} files cite them:")
    for p in sorted(changed):
        print(f"  {p.relative_to(ROOT)}")
    if not a.apply:
        return
    for n, sub in moves.items():
        (folder / sub).mkdir(exist_ok=True)
        subprocess.run(["git", "mv", f"{rel}/{n}", f"{rel}/{sub}/{n}"], cwd=ROOT, check=True)
    for p, t in changed.items():
        if p.parent == folder and p.name in moves:  # a moved file that cites itself: written at its new place
            p = folder / moves[p.name] / p.name
        with p.open("w", encoding="utf-8", newline="") as f:
            f.write(t)


if __name__ == "__main__":
    main()
