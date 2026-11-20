"""The setup's search for the player's copies (tools/prepare/finddumps.py), for Steam's Gaming Mode where no file dialog
opens: a Mario Kart World .nup found with its version from the package's table, another game's .nup and an unknown
version told apart, a .nx package and a loose exefs/ + romfs/ copy of a known build found, a copy of an unknown build
and the installation folder itself left out, and the search stopping at its depth. Seconds, on Linux and Windows.

Usage: uv run pytest tests
"""

import struct
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

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
    # the newest of the game's titles by version, wherever it sits; a DLC (another id) last does not hide the game
    nup(tmp_path / "Games/MKW eglantine.nup", [(MKW, 917504), (MKW | 0x800, 0), (MKW | 0x1001, 1 << 16)])
    (tmp_path / "Games/broken.nup").write_bytes(b"NUP\x01")
    assert found([tmp_path]) == {
        ("nup", "1.8.0", "MKW 1.8.0.nup"),
        ("nup", "?", "MKW new.NUP"),
        ("nup", "1.8.0", "MKW eglantine.nup"),
    }


def test_nx_and_loose_copies(tmp_path: Path) -> None:
    (tmp_path / "card").mkdir()
    (tmp_path / "card/mkw.nx").write_bytes(b"NXAR" + b"\0" * 60)
    (tmp_path / "card/notes.nx").write_bytes(b"text")
    loose(tmp_path / "dumps/MKW", BUILD_180)
    loose(tmp_path / "dumps/Other", bytes(8))
    assert found([tmp_path]) == {("nx", "?", "mkw.nx"), ("folder", "1.8.0", "MKW")}


@pytest.mark.parametrize("name", ["Plain", "Café", "玩家"])
def test_cli_emits_utf8_dump_paths(tmp_path: Path, name: str) -> None:
    path = nup(tmp_path / f"{name}.nup", [(MKW, 0)])
    # -I ignores PYTHONIOENCODING. Give the CLI the same initial pipe encoding as cp1252 Windows explicitly.
    driver = (
        "import runpy,sys; sys.stdout.reconfigure(encoding='cp1252'); "
        "sys.argv=sys.argv[1:]; runpy.run_path(sys.argv[0],run_name='__main__')"
    )
    result = subprocess.run(
        [sys.executable, "-I", "-c", driver, str(ROOT / "tools/prepare/finddumps.py"), str(tmp_path)],
        capture_output=True, timeout=10,
    )
    assert result.returncode == 0, result.stderr.decode("utf-8", "replace")
    assert result.stdout.decode("utf-8").splitlines() == [f"dump\tnup\t1.0.0\t{path}", "done 1"]


def test_installation_hidden_and_deep_folders_left_out(tmp_path: Path) -> None:
    game = loose(tmp_path / "mkw-recomp", BUILD_180)
    nup(tmp_path / ".cache/MKW.nup", [(MKW, 0)])
    nup(tmp_path / "a/b/c/d/e/MKW.nup", [(MKW, 0)])  # five folders below the root, past DEPTH
    nup(tmp_path / "a/b/c/MKW.nup", [(MKW, 0)])
    assert found([tmp_path], game) == {("nup", "1.0.0", "MKW.nup")}


def test_result_cap_inside_one_directory(tmp_path: Path) -> None:
    for i in range(finddumps.MAX_FOUND + 4):
        (tmp_path / f"{i}.nx").write_bytes(b"NXAR")
    rows = []
    count = finddumps.search([tmp_path], None, 10, lambda *row: rows.append(row))
    assert count == len(rows) == finddumps.MAX_FOUND


def test_deadline_inside_one_directory(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    for i in range(5):
        (tmp_path / f"{i}.nx").write_bytes(b"NXAR")
    scandir = finddumps.os.scandir
    scan = scandir(tmp_path)
    consumed = 0
    closed = False

    class TimedScan:
        def __enter__(self):
            return self

        def __exit__(self, *_):
            nonlocal closed
            scan.close()
            closed = True

        def __iter__(self):
            return self

        def __next__(self):
            nonlocal consumed
            entry = next(scan)
            consumed += 1  # one simulated second per entry fetched
            return entry

    monkeypatch.setattr(finddumps, "os", SimpleNamespace(
        path=finddumps.os.path, scandir=lambda _: TimedScan(),
    ))
    monkeypatch.setattr(finddumps, "time", SimpleNamespace(monotonic=lambda: consumed))
    rows = []
    try:
        count = finddumps.search([tmp_path], None, 2, lambda *row: rows.append(row))
    finally:
        scan.close()
    assert count == len(rows) == consumed == 2
    assert closed
