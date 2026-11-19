"""The mod merger must rebuild when the same enabled mods use a different original RomFS."""

import hashlib
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/mod"))
import merge  # noqa: E402
from nup import pack  # noqa: E402


@pytest.mark.parametrize("packed", [False, True])
def test_merge_cache_tracks_original_romfs(tmp_path: Path, packed: bool) -> None:
    game = tmp_path / "game"
    a, b = tmp_path / "version A", tmp_path / "version B"
    originals = []
    sources = []
    for source, data in [(a, b"old original"), (b, b"new original")]:
        romfs = source / "romfs"
        romfs.mkdir(parents=True)
        raw = merge.build_sarc([("original.bin", data)])
        (romfs / "shared.sarc").write_bytes(raw)
        originals.append(raw)
        if packed:
            (source / "exefs").mkdir()
            (source / "exefs/main").write_bytes(b"synthetic header")
            package = source.with_suffix(".nup")
            pack(source, package, 0x0400C3F00006E000, 0)
            sources.append(package)
        else:
            sources.append(romfs)

    for mod, filename in [("A", "first.bin"), ("B", "second.bin")]:
        romfs = game / "mods" / mod / "romfs"
        romfs.mkdir(parents=True)
        (romfs / "shared.sarc").write_bytes(merge.build_sarc([(filename, b"modded")]))

    def run(romfs: Path) -> str:
        result = subprocess.run([sys.executable, "-I", str(ROOT / "tools/mod/merge.py"),
                                 "--game", str(game), "--romfs", str(romfs), "--enable", "A,B"],
                                check=True, capture_output=True, text=True, encoding="utf-8", timeout=30)
        return result.stdout.strip()

    assert run(sources[0]).startswith("done ")
    assert run(sources[0]) == "fresh"
    assert run(sources[1]).startswith("done ")
    merged = game / "mods/.merged/romfs"
    entries = dict(merge.parse_sarc_ordered((merged / "shared.sarc").read_bytes())[0])
    assert entries == {"original.bin": b"new original", "first.bin": b"modded", "second.bin": b"modded"}
    manifest = (merged / "manifest.tsv").read_text(encoding="utf-8")
    assert manifest.strip().split("\t")[-1] == hashlib.sha256(originals[1]).hexdigest()
    assert run(sources[1]) == "fresh"
