"""The setup's command lines and files without the game: paths with spaces and accents, the resume without --source and
the update's checks. Seconds, on Linux and on Windows (ci.yml), so a break like issue #29 (a compiler path split at its
space, found by a player) shows in the commit that makes it.

Usage: uv run pytest tests
"""

import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "sm86/spec"))
sys.path.insert(0, str(ROOT / "tools/prepare"))
import prepare  # noqa: E402
import spec  # noqa: E402
import update  # noqa: E402
from versions import VERSIONS  # noqa: E402  (build.py, imported by prepare, put recomp/ on the path)

ODD = "Recomp C ñ"  # a space and an accent, as the players' folders have


def test_cc_survives_spaces(tmp_path: Path) -> None:
    # the shaders step's command line as prepare.py builds it, read back as spec.py's argparse does
    cc = [str(tmp_path / ODD / "zig.exe"), "cc", "-target", "x86_64-windows-gnu"]
    cmd = prepare.spec_command(tmp_path / ODD, tmp_path / ODD / "work", cc, None)
    assert spec.cc_command(cmd[cmd.index("--cc") + 1]) == cc
    assert spec.cc_command("zig cc") == ["zig", "cc"]  # by hand


def run_prepare(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(ROOT / "tools/prepare/prepare.py"), *args],
                          capture_output=True, text=True, timeout=60)  # fmt: skip


def test_resume_without_source(tmp_path: Path) -> None:
    game = tmp_path / ODD / "game"
    game.mkdir(parents=True)
    copy = tmp_path / ODD / "copy"
    state = {"version": 1, "done": {"exefs": str(copy / "exefs"), "romfs": f"link {copy / 'romfs'}"}}
    (game / "setup-state.json").write_text(json.dumps(state))
    p = run_prepare("--game", str(game))
    # the last setup's copy, whole: check fails on its missing files, naming the folder with its space
    assert "step check" in p.stdout, p.stdout + p.stderr
    assert str(copy) in p.stdout, p.stdout
    saved = json.loads((game / "setup-state.json").read_text())
    assert saved.get("source", str(copy)) == str(copy)


def test_resume_without_earlier_setup(tmp_path: Path) -> None:
    p = run_prepare("--game", str(tmp_path / ODD))
    assert p.returncode == 2 and "error source" in p.stdout, p.stdout


def test_versions() -> None:
    assert update.version_key("0.1.2-12-g1c15097") == (0, 1, 2)
    assert update.version_key("v0.1.10") > update.version_key("0.1.9")
    assert update.version_key("dev") is None


@pytest.mark.parametrize("name", ["stale.old", "setup.old"])
def test_clean_removes_old(tmp_path: Path, name: str) -> None:
    game = tmp_path / ODD
    (game / "update").mkdir(parents=True)
    old = game / name
    old.mkdir() if name == "setup.old" else old.write_text("x")
    (game / "mkw.exe").write_text("keep")
    update.clean(type("A", (), {"game": game})())
    assert not old.exists() and not (game / "update").exists() and (game / "mkw.exe").exists()


def test_package_sources_import(tmp_path: Path) -> None:
    # setup/src as dist.py copies it: prepare.py and what it imports load without the rest of the repository (build.py
    # imported tools/i18n/catalog.py, which the package lacked)
    sys.path.insert(0, str(ROOT / "tools/build"))
    import dist  # noqa: PLC0415

    dist.copy_sources(tmp_path / "src")
    p = subprocess.run([sys.executable, "-I", str(tmp_path / "src/tools/prepare/prepare.py"), "--help"],
                       capture_output=True, text=True, timeout=60)  # fmt: skip
    assert p.returncode == 0, p.stderr


def test_find_game_packages(tmp_path: Path) -> None:
    # a .nup or .nx package is the source as one file (its exefs and romfs both); any other file is refused
    for name, magic in (("g.nup", b"NUP\x01"), ("g.nx", b"NXAR")):
        (tmp_path / name).write_bytes(magic + bytes(60))
        assert prepare.find_game(tmp_path / name) == (tmp_path / name, tmp_path / name)
    (tmp_path / "g.zip").write_bytes(b"PK\x03\x04" + bytes(60))
    with pytest.raises(prepare.Failed):
        prepare.find_game(tmp_path / "g.zip")


@pytest.mark.parametrize("name", ["1.0.0", "1.8.0"])
def test_check_package_source(tmp_path: Path, name: str) -> None:
    # the setup from a .nup: check() reads exefs/main and the RomFS's file count from the package's table of
    # contents (nothing extracted) and picks the version row (recomp/versions.py) by the build id
    import test_nup

    v = {x.name: x for x in VERSIONS}[name]
    need = int(v.romfs_files * prepare.ROMFS_MARGIN)
    main = b"NSO0" + b"\x00" * (0x40 - 4) + bytes.fromhex(v.main_build_id)
    p = test_nup.write_nup(tmp_path, [test_nup.pkg(1, 0xE0000, 0x0400C3F00006E000, exefs=[("main", main, 0)],
                                                      romfs=[(f"f{i}", b"", 0) for i in range(need)], flags=1)])
    assert prepare.check(p, tmp_path) == (p, p, v)
