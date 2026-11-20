"""A mod archive whose SARC header is cut short is served whole, like any file that is not an archive: it must not end
the merge of the other mods (parse_sarc_ordered raises struct.error there, which only ValueError handlers caught).

Usage: uv run pytest tests/launcher/test_mod_merge_damaged.py
"""

import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/mod"))
import merge  # noqa: E402

CUT = b"SARC\x14\x00\xff\xfe"  # the magic and the byte order mark, then nothing


def run(game: Path, romfs: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-I", str(ROOT / "tools/mod/merge.py"), "--game", str(game), "--romfs", str(romfs),
         "--enable", "A,B"],
        capture_output=True, text=True, encoding="utf-8", timeout=30, check=False,
    )  # fmt: skip


def make(tmp_path: Path, *, damaged_in_mods: bool, damaged_original: bool) -> tuple[Path, Path]:
    game, romfs = tmp_path / "game", tmp_path / "original" / "romfs"
    romfs.mkdir(parents=True)
    (romfs / "good.sarc").write_bytes(merge.build_sarc([("a.bin", b"original")]))
    if damaged_original:
        (romfs / "cut.sarc").write_bytes(CUT)
    for mod, name in (("A", "first.bin"), ("B", "second.bin")):
        mod_romfs = game / "mods" / mod / "romfs"
        mod_romfs.mkdir(parents=True)
        (mod_romfs / "good.sarc").write_bytes(merge.build_sarc([("a.bin", b"original"), (name, b"modded")]))
        if damaged_in_mods:
            (mod_romfs / "cut.sarc").write_bytes(CUT)
        elif damaged_original:  # the mods' own copies are fine: only the game's file to compare with is cut
            (mod_romfs / "cut.sarc").write_bytes(merge.build_sarc([(name, b"modded")]))
    return game, romfs


@pytest.mark.parametrize(("in_mods", "in_original"), [(True, False), (False, True)])
def test_cut_archive_does_not_stop_the_merge(tmp_path: Path, in_mods: bool, in_original: bool) -> None:
    game, romfs = make(tmp_path, damaged_in_mods=in_mods, damaged_original=in_original)
    result = run(game, romfs)
    assert result.returncode == 0, result.stderr[-400:]
    assert result.stdout.strip().splitlines()[-1].startswith("done merged=")
    merged = merge.parse_sarc_ordered((game / "mods/.merged/romfs/good.sarc").read_bytes())[0]
    assert dict(merged) == {"a.bin": b"original", "first.bin": b"modded", "second.bin": b"modded"}
