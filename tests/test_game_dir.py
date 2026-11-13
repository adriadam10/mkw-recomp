"""Installation selection survives launcher restarts and isolates settings and prepared-library discovery.

Why: the game library is found before mem_init, and selecting a directory must save only the selector beside the
launcher. Compile cfg.c and gamelib.c with a small POSIX file harness; no game, SDL or GPU is needed.
Usage: uv run pytest tests/test_game_dir.py
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
pytestmark = pytest.mark.skipif(os.name == "nt", reason="POSIX file harness; setup tests also run on Windows")


@pytest.fixture(scope="module")
def game_dir_probe(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("no C compiler")
    exe = tmp_path_factory.mktemp("game-dir") / "probe"
    inc = [f"-I{ROOT / 'runtime'}", f"-I{ROOT / 'runtime/cpu'}", f"-I{ROOT / 'runtime/core/base'}",
           f"-I{ROOT / 'runtime/core/boot'}"]
    subprocess.run([cc, *inc, str(ROOT / "tests/c/t_game_dir.c"), str(ROOT / "runtime/core/base/cfg.c"),
                    str(ROOT / "runtime/core/boot/gamelib.c"), "-o", str(exe)], check=True)  # fmt: skip
    return exe


def probe(exe: Path, *args: str, **overrides: str) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("MKW_")}
    env.update(overrides)
    p = subprocess.run([str(exe), *args], env=env, check=True, capture_output=True, text=True)
    return dict(line.split("=", 1) for line in p.stdout.splitlines())


@pytest.fixture
def launcher(game_dir_probe: Path, tmp_path: Path) -> Path:
    folder = tmp_path / "Launcher ñ with spaces"
    folder.mkdir()
    return Path(shutil.copy2(game_dir_probe, folder / "mkw"))


def installation(folder: Path, volume: int = 80) -> Path:
    folder.mkdir()
    (folder / "mkw.ini").write_text(f"romfs = /dump ñ/romfs\nvolume = {volume}\n")
    (folder / "mkwgame.so").touch()  # readiness checks the id without loading code, as the launcher does
    (folder / "mkwgame.id").write_text("dev")
    return folder


def test_default_and_persisted_directory(launcher: Path, tmp_path: Path) -> None:
    assert probe(launcher)["dir"] == str(launcher.parent)
    game = installation(tmp_path / "Games ñ with spaces")
    assert probe(launcher, "select", str(game))["selected"] == "1"
    assert probe(launcher) == {"dir": str(game), "ready": "1", "romfs": "/dump ñ/romfs", "volume": "80"}
    selector = (launcher.parent / "mkw.ini").read_text()
    probe(launcher, "save", "41")
    assert probe(launcher)["volume"] == "41"
    assert (launcher.parent / "mkw.ini").read_text() == selector
    assert probe(launcher, "select", str(launcher.parent))["selected"] == "1"
    assert probe(launcher)["dir"] == str(launcher.parent)
    assert probe(launcher)["volume"] == "100"


def test_relative_directory_and_environment_override(launcher: Path, tmp_path: Path) -> None:
    game = installation(launcher.parent / "Installed game")
    (launcher.parent / "mkw.ini").write_text("game_dir = Installed game\n")
    assert probe(launcher)["dir"] == str(game)
    other = installation(tmp_path / "Other game", 65)
    assert probe(launcher, MKW_GAME_DIR=str(other), MKW_VOLUME="32")["volume"] == "32"
    result = probe(launcher, "select", str(game), MKW_GAME_DIR=str(other))
    assert result["dir"] == str(other) and result["selected"] == "0"
    assert probe(launcher)["dir"] == str(game)


def test_switch_to_empty_folder_does_not_reuse_old_installation(launcher: Path, tmp_path: Path) -> None:
    installation(launcher.parent / "old")
    probe(launcher, "select", str(launcher.parent / "old"))
    empty = tmp_path / "Empty installation"
    empty.mkdir()
    assert probe(launcher, "select", str(empty))["selected"] == "1"
    assert probe(launcher) == {"dir": str(empty), "ready": "0", "romfs": "", "volume": "100"}


def test_failed_selection_keeps_current_installation(launcher: Path, tmp_path: Path) -> None:
    game = installation(tmp_path / "Current installation")
    probe(launcher, "select", str(game))
    assert probe(launcher, "fail", str(tmp_path / "Other"))["selected"] == "0"
    result = probe(launcher)
    assert result["dir"] == str(game) and result["volume"] == "37"
    assert "game_dir" not in (game / "mkw.ini").read_text()
