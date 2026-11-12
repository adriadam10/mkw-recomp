"""The game versions this port knows, keyed by the build id of exefs/main.

Why: each update of Mario Kart World is a different executable (other addresses, another sdk build, 1.8.0 adds
subsdk1, the nnEcli module), so everything tied to one build (the patch sites, the RomFS size the setup checks, the
number the runtime switches its fixed addresses on) is chosen here, from the dump itself, in one place. The runtime
keeps the matching rows for its own fixed addresses in runtime/core/boot/version.c, keyed by `number`.

Usage: from versions import detect; v = detect(exefs)  ->  Version, or ValueError for a build not listed.
       uv run recomp/versions.py [EXEFS]   prints the version of a dump.
"""

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


def main_build_id(exefs: Path) -> str:
    head = (Path(exefs) / "main").read_bytes()[:0x48]
    if head[:4] != b"NSO0":
        raise ValueError(f"{Path(exefs) / 'main'} is not an NSO: decrypted exefs needed")
    return head[0x40:0x48].hex()


def detect(exefs: Path) -> Version:
    got = main_build_id(exefs)
    for v in VERSIONS:
        if v.main_build_id == got:
            return v
    names = ", ".join(v.name for v in VERSIONS)
    raise ValueError(f"exefs/main is not a known version of Mario Kart World ({names}; build id {got})")


def modules(exefs: Path) -> list[str]:
    """The game modules present in the ExeFS, in load order (recomp/layout.py ORDER)."""
    sys.path.insert(0, str(ROOT / "recomp"))
    from layout import ORDER

    return [m for m in ORDER if (Path(exefs) / m).is_file()]


if __name__ == "__main__":
    exefs = Path(sys.argv[1] if len(sys.argv) > 1 else "game/exefs")
    v = detect(exefs)
    print(f"Mario Kart World {v.name} (version {v.number}), modules {', '.join(modules(exefs))}, patches {v.patches.name}")
