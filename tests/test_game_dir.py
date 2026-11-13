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
                    str(ROOT / "runtime/core/base/sysinfo_log.c"),
                    str(ROOT / "runtime/core/boot/gamelib.c"), "-o", str(exe)], check=True)  # fmt: skip
    return exe


def probe(exe: Path, *args: str, **overrides: str) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MKW_", "TEST_LOG_", "TEST_STDERR_"))}
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


def nested_path(base: Path, size: int) -> Path:
    """A real POSIX path with the requested UTF-8 byte length, with each component below NAME_MAX."""
    path = base
    while size - len(str(path).encode()) > 201:
        path /= "x" * 200
    remaining = size - len(str(path).encode()) - 1
    assert remaining > 0
    return path / ("ñ" * (remaining // 2) + "x" * (remaining % 2))


@pytest.mark.parametrize("ending", [" ", "\t", "  \t", '" ', '\\ '])
def test_selection_preserves_whitespace(launcher: Path, tmp_path: Path, ending: str) -> None:
    for name in ("First", "Second"):
        game = installation(tmp_path / (name + ending))
        result = probe(launcher, "select", str(game))
        assert result["selected"] == "1" and result["value"] == str(game)
        assert f'game_dir = "{game}"\n' in (launcher.parent / "mkw.ini").read_text()
        assert probe(launcher)["dir"] == str(game)
        assert probe(launcher)["ready"] == "1"


@pytest.mark.parametrize("size", [1155, 2048])
def test_rejected_directory_does_not_replace_selection(launcher: Path, tmp_path: Path, size: int) -> None:
    game = installation(tmp_path / "Current")
    probe(launcher, "select", str(game))
    selector = (launcher.parent / "mkw.ini").read_bytes()
    long_path = nested_path(tmp_path, size)
    long_path.mkdir(parents=True)
    result = probe(launcher, "select", str(long_path))
    assert result["selected"] == "0" and result["invalid"] == "1"
    assert (launcher.parent / "mkw.ini").read_bytes() == selector
    assert probe(launcher)["dir"] == str(game)


def test_supported_path_boundary_and_relative_resolution(launcher: Path) -> None:
    limit = int(probe(launcher, "limit")["limit"])
    game = nested_path(launcher.parent, limit)
    game.parent.mkdir(parents=True)
    installation(game)
    assert len(str(game)) < limit  # bytes, rather than Unicode characters, determine the limit
    assert probe(launcher, "select", str(game))["selected"] == "1"
    assert probe(launcher)["dir"] == str(game)
    assert probe(launcher)["ready"] == "1"
    too_long = str(game) + "x"
    relative = os.path.relpath(too_long, launcher.parent)
    assert len(relative.encode()) < limit
    assert probe(launcher, "select", relative)["invalid"] == "1"
    assert probe(launcher)["dir"] == str(game)


def test_invalid_saved_directory_allows_recovery(launcher: Path, tmp_path: Path) -> None:
    invalid = nested_path(tmp_path, 2048)
    ini = launcher.parent / "mkw.ini"
    ini.write_text(f'game_dir = "{invalid}"\n')
    assert probe(launcher)["dir"] == str(launcher.parent)
    game = installation(tmp_path / "Recovered")
    assert probe(launcher, "select", str(game))["selected"] == "1"
    assert probe(launcher)["dir"] == str(game)


@pytest.mark.parametrize("path", ["/" + "x" * 1154, "/games/Line\nbreak"])
def test_invalid_selection_does_not_create_selector(launcher: Path, path: str) -> None:
    result = probe(launcher, "select", path)
    assert result["selected"] == "0" and result["invalid"] == "1"
    assert result["value"] == ""
    assert not (launcher.parent / "mkw.ini").exists()


def test_relative_selection_is_saved_as_supported_absolute_path(launcher: Path) -> None:
    game = installation(launcher.parent / "Relative installation ")
    result = probe(launcher, "select", "Relative installation ")
    assert result["selected"] == "1" and result["value"] == str(game)
    assert probe(launcher)["dir"] == str(game)


def test_log_folder_follows_installation(launcher: Path, tmp_path: Path) -> None:
    assert probe(launcher, "logs") == {"ok": "1", "folder": str(launcher.parent / "logs")}
    game = installation(tmp_path / "External installation ñ ")
    probe(launcher, "select", str(game))
    assert probe(launcher, "logs") == {"ok": "1", "folder": str(game / "logs")}


def test_log_folder_supports_long_installation_path(launcher: Path) -> None:
    limit = int(probe(launcher, "limit")["limit"])
    game = nested_path(launcher.parent, limit)
    game.parent.mkdir(parents=True)
    installation(game)
    probe(launcher, "select", str(game))
    assert probe(launcher, "logs") == {"ok": "1", "folder": str(game / "logs")}


@pytest.mark.parametrize(("log", "redirected", "folder"), [
    ("/captures/game.log", "/redirected/run.log", "/captures"),
    (None, "/redirected/run.log", "/redirected"),
    (r"C:\captures\game.log", None, r"C:\captures"),
    ("/game.log", None, "/"),
    (r"C:\game.log", None, "C:\\"),
])
def test_log_folder_preserves_active_destination(
    launcher: Path, tmp_path: Path, log: str | None, redirected: str | None, folder: str
) -> None:
    game = installation(tmp_path / "External installation")
    probe(launcher, "select", str(game))
    overrides = {}
    if log:
        overrides["TEST_LOG_PATH"] = log
    if redirected:
        overrides["TEST_STDERR_PATH"] = redirected
    assert probe(launcher, "logs", **overrides) == {"ok": "1", "folder": folder}


@pytest.mark.parametrize("active_log", [False, True])
def test_log_folder_rejects_truncated_path(launcher: Path, active_log: bool) -> None:
    overrides = {"TEST_LOG_PATH": "/a/long/log/path/run.log"} if active_log else {}
    assert probe(launcher, "logs", "8", **overrides)["ok"] == "0"
