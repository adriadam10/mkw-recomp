"""The game versions this port knows, keyed by the build id of exefs/main.

Why: each update of Mario Kart World is a different executable (other addresses, another sdk build, 1.8.0 adds
subsdk1, the nnEcli module), so everything tied to one build (the patch sites, the RomFS size the setup checks, the
number the runtime switches its fixed addresses on) is chosen here, from the dump itself, in one place. The runtime
keeps the matching rows for its own fixed addresses in runtime/core/boot/version.c, keyed by `number`.

Usage: from versions import detect; v = detect(exefs)  ->  Version, or ValueError for a build not listed.
       uv run recomp/versions.py [EXEFS]   prints the version of a dump.
       uv run recomp/versions.py --gen-dir EXEFS   prints the generated-C dir for the dump (build/generated-180).
       uv run recomp/versions.py --patches EXEFS   prints the patch table for the dump.
"""

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Version:
    name: str  # as the game shows it: 1.0.0
    number: int  # RtGame.version (runtime/core/boot/gamelib.h): 100, 180
    main_build_id: str  # first 8 bytes of the NSO build id of exefs/main, hex
    romfs_files: int  # files of its RomFS, counted on a dump
    patches: Path  # recomp/patches-*.txt for this executable


VERSIONS = (
    Version("1.0.0", 100, "9e244438c3d8ef1c", 205_660, ROOT / "recomp/patches-mkw.txt"),  # 06/11/2026
    Version("1.8.0", 180, "d00e4f399e411d06", 206_202, ROOT / "recomp/patches-mkw-180.txt"),  # 11/11/2026
)


def build_id_of(head: bytes, where: str = "exefs/main") -> str:
    """The build id of exefs/main from its first 0x48 bytes (a folder's file or a .nup package's entry)."""
    if head[:4] != b"NSO0":
        raise ValueError(f"{where} is not an NSO: decrypted exefs needed")
    return head[0x40:0x48].hex()


def main_build_id(exefs: Path) -> str:
    return build_id_of((Path(exefs) / "main").read_bytes()[:0x48], str(Path(exefs) / "main"))


def detect(exefs: Path) -> Version:
    return detect_id(main_build_id(exefs))


def detect_id(got: str) -> Version:
    for v in VERSIONS:
        if v.main_build_id == got:
            return v
    names = ", ".join(v.name for v in VERSIONS)
    raise ValueError(f"exefs/main is not a known version of Mario Kart World ({names}; build id {got})")


# The RomFS names its game data version in one file: System/Resource/ResourceSizeTable.Product.<number>.*.rsizetable.zs
# (100 in 1.0.0's, 180 in 1.8.0's). An executable opens its own number's files: 1.0.0's main with 1.8.0's RomFS (#315)
# passed the file count and crashed reading a Gyml parameter (main+0x78ab50) while warming up.
ROMFS_PRODUCT = "System/Resource/ResourceSizeTable.Product."


def romfs_products(names) -> set[int]:
    """The game data version numbers RomFS file paths (relative, '/'-separated) name, empty when none does."""
    found = set()
    for name in names:
        if name.startswith(ROMFS_PRODUCT):
            number = name[len(ROMFS_PRODUCT) :].split(".", 1)[0]
            if number.isdigit():
                found.add(int(number))
    return found


def romfs_mismatch(v: Version, names) -> str:
    """Why the RomFS whose file paths are `names` does not belong to v ("" when it does or names no version)."""
    found = romfs_products(names)
    if not found or v.number in found:
        return ""
    known = {x.number: x.name for x in VERSIONS}
    got = ", ".join(known.get(n, f"Product.{n}") for n in sorted(found))
    return f"RomFS {got}, exefs/main {v.name}"


def modules(exefs: Path) -> list[str]:
    """The game modules present in the ExeFS, in load order (recomp/layout.py ORDER)."""
    sys.path.insert(0, str(ROOT / "recomp"))
    from layout import ORDER

    return [m for m in ORDER if (Path(exefs) / m).is_file()]


def gen_dir_for(v: Version) -> Path:
    """The generated-C dir for a version (1.0.0 keeps build/generated, later versions add their number)."""
    return ROOT / "build/generated" if v.number == 100 else ROOT / f"build/generated-{v.number}"


def default_gen(exefs: Path) -> Path:
    """The generated-C dir for the dump in exefs (from its build id)."""
    return gen_dir_for(detect(exefs))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Name the game version of a dump.")
    ap.add_argument("exefs", nargs="?", type=Path, default=Path("game/exefs"))
    ap.add_argument("--gen-dir", action="store_true", help="print only the generated-C dir for the dump")
    ap.add_argument("--patches", action="store_true", help="print only the patch table for the dump")
    ap.add_argument("--number", action="store_true", help="print only the RtGame.version number for the dump")
    args = ap.parse_args()
    v = detect(args.exefs)
    if args.gen_dir:
        print(gen_dir_for(v))
    elif args.patches:
        print(v.patches)
    elif args.number:
        print(v.number)
    else:
        print(f"Mario Kart World {v.name} (version {v.number}), modules {', '.join(modules(args.exefs))}, patches {v.patches.name}")
