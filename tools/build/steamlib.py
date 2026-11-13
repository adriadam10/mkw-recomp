"""Put Steam's runtime library next to mkw, for Steam Remote Play Together (runtime/steam/, docs/remote-play.md).

Why: the runtime loads libsteam_api.so / steam_api64.dll at run time from its own folder and links nothing of Valve's,
so neither the build nor the release carries it. This takes it from the Steamworks.NET 2025.165.0 standalone archive,
whose copies are byte-identical to Valve's Steamworks SDK 1.65 redistributable_bin/ (no Steamworks login needed); the
archive and the library are checked against pinned sha256s, the same ones as the Eglantine Remote Play Together
extension's release workflow (13/11/2026: both checked by hand against a download of that archive).

Usage: uv run tools/build/steamlib.py [--target linux|windows] [--dest DIR]
       DIR: the folder holding mkw or mkw.exe (default build/dist/<target>). The Linux AppImage runs its copy of mkw in
       ~/.local/share/mkw-recomp: pass that folder there.
"""

import argparse
import shutil
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dist import fetch, sha256  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE = (
    "https://github.com/rlabrecque/Steamworks.NET/releases/download/2025.165.0/Steamworks.NET-Standalone_2025.165.0.zip",
    "9b86dda48b257013a350c8fea87edf3ab3f6f8fd6a812563fddec73fb1189fee",
)
# target: (path in the archive, sha256)
LIBRARY = {
    "linux": ("OSX-Linux-x64/libsteam_api.so", "eb2dd015b84177cf4f4326fe578aab375fd8931bbbd719c7492420d9777007fe"),
    "windows": ("Windows-x64/steam_api64.dll", "e6d9bafb9a41e42fba7b21553db49f8719027af3f0deb23ff86cfc44d60e776d"),
}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--target", choices=tuple(LIBRARY), default="linux")
    ap.add_argument("--dest", type=Path, help="the folder of mkw (default build/dist/<target>)")
    args = ap.parse_args()
    dest = args.dest or ROOT / "build/dist" / args.target
    member, want = LIBRARY[args.target]
    archive = fetch(*ARCHIVE)
    dest.mkdir(parents=True, exist_ok=True)
    out = dest / Path(member).name
    part = out.with_name(out.name + ".part")
    with zipfile.ZipFile(archive) as z, z.open(member) as src, part.open("wb") as f:
        shutil.copyfileobj(src, f)
    got = sha256(part)
    if got != want:
        part.unlink()
        sys.exit(f"{member}: sha256 {got}, expected {want}")
    part.replace(out)
    print(f"{out} (Steamworks SDK 1.65)")


if __name__ == "__main__":
    main()
