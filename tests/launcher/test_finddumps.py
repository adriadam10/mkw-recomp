"""The setup's search for the player's copies (tools/prepare/finddumps.py), for Steam's Gaming Mode where no file dialog
opens: a Mario Kart World .nup found with its version from the package's table, another game's .nup and an unknown
version told apart, a .nx package and a loose exefs/ + romfs/ copy of a known build found, a copy of an unknown build
and the installation folder itself left out, and the search stopping at its depth. Seconds, on Linux and Windows.

Usage: uv run pytest tests
"""

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/prepare"))
import finddumps  # noqa: E402

MKW = 0x0400C3F00006E000
BUILD_180 = bytes.fromhex("d00e4f399e411d06")


def nup(path: Path, titles: list[tuple[int, int]]) -> Path:
    """A .nup with only what the search reads: the magic, the table's offset and its title records (nup.py's layout)."""
    toc = 0x40
    head = b"NUP\x01" + b"\0" * 4 + struct.pack("<I", toc)
    table = b"TOC\x01" + struct.pack("<I", len(titles)) + b"\0" * 0x28
    for tid, version in titles:
        table += struct.pack("<QIIII", tid, 0, 1, 2, version).ljust(0x40, b"\0")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(head.ljust(toc, b"\0") + table)
    return path


def loose(path: Path, build: bytes) -> Path:
    (path / "exefs").mkdir(parents=True)
    (path / "romfs").mkdir()
    (path / "exefs/main").write_bytes(b"NSO0" + b"\0" * 0x3C + build + b"\0" * 64)
    return path


def found(roots: list[Path], game: Path | None = None) -> set[tuple[str, str, str]]:
    out = set()
    finddumps.search(roots, game, 10.0, lambda kind, version, path: out.add((kind, version, Path(path).name)))
    return out


def test_nup_versions_and_titles(tmp_path: Path) -> None:
    nup(tmp_path / "Downloads/MKW 1.8.0.nup", [(MKW, 0), (MKW, 917504)])  # base game then update: the newest counts
    nup(tmp_path / "Downloads/other.nup", [(0x0100000000010000, 0)])
    nup(tmp_path / "Games/MKW new.NUP", [(MKW, 1 << 20)])
    (tmp_path / "Games/broken.nup").write_bytes(b"NUP\x01")
    assert found([tmp_path]) == {("nup", "1.8.0", "MKW 1.8.0.nup"), ("nup", "?", "MKW new.NUP")}


def test_nx_and_loose_copies(tmp_path: Path) -> None:
    (tmp_path / "card").mkdir()
    (tmp_path / "card/mkw.nx").write_bytes(b"NXAR" + b"\0" * 60)
    (tmp_path / "card/notes.nx").write_bytes(b"text")
    loose(tmp_path / "dumps/MKW", BUILD_180)
    loose(tmp_path / "dumps/Other", bytes(8))
    assert found([tmp_path]) == {("nx", "?", "mkw.nx"), ("folder", "1.8.0", "MKW")}


def test_installation_hidden_and_deep_folders_left_out(tmp_path: Path) -> None:
    game = loose(tmp_path / "mkw-recomp", BUILD_180)
    nup(tmp_path / ".cache/MKW.nup", [(MKW, 0)])
    nup(tmp_path / "a/b/c/d/e/MKW.nup", [(MKW, 0)])  # five folders below the root, past DEPTH
    nup(tmp_path / "a/b/c/MKW.nup", [(MKW, 0)])
    assert found([tmp_path], game) == {("nup", "1.0.0", "MKW.nup")}
