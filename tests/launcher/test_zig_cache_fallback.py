"""zig's cache folder moves to the temporary folder when zig cannot use the game folder's (#406: every chunk failed with
`checking cache failed: manifest_create Unexpected` on a game folder run through GE-Proton on Batocera), with a fake
compiler that refuses the cache folders the test names. Seconds, no game.

Usage: uv run pytest tests/launcher/test_zig_cache_fallback.py
"""

import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/prepare"))
import machine  # noqa: E402

# A compiler whose cache folder (ZIG_GLOBAL_CACHE_DIR) the test may refuse: argv[1] lists the folder names it refuses.
FAKE_ZIG = """
import os, sys
from pathlib import Path
refused = sys.argv[1].split(",")
cache = os.environ.get("ZIG_GLOBAL_CACHE_DIR", "")
Path(sys.argv[2]).open("a").write("x")
if Path(cache).name in refused:
    sys.stderr.write("error: unable to build: checking cache failed: manifest_create Unexpected\\n")
    sys.exit(1)
if "-c" not in sys.argv:
    sys.exit(2)
Path(sys.argv[sys.argv.index("-o") + 1]).write_bytes(b"obj")
"""


@pytest.fixture
def setup(monkeypatch: pytest.MonkeyPatch, tmp_path: Path):
    game, probe = tmp_path / "game", tmp_path / "obj"
    probe.mkdir()
    own = game / "setup-work" / machine.ZIG_CACHE
    own.mkdir(parents=True)
    monkeypatch.setattr(machine.tempfile, "gettempdir", lambda: str(tmp_path / "tmp"))
    (tmp_path / "tmp").mkdir()
    monkeypatch.setenv("ZIG_GLOBAL_CACHE_DIR", str(own))
    monkeypatch.setenv("ZIG_LOCAL_CACHE_DIR", str(own))
    calls = tmp_path / "calls"
    fake = tmp_path / "fake_zig.py"
    fake.write_text(FAKE_ZIG)

    def cc(refused: str) -> list[str]:
        return [sys.executable, str(fake), refused, str(calls)]

    return game, probe, own, tmp_path / "tmp" / "mkw-zig-cache", cc, calls


def test_cache_moves_when_the_game_folder_refuses_it(setup, capsys: pytest.CaptureFixture[str]) -> None:
    game, probe, own, other, cc, _ = setup
    machine.settle_zig_cache(cc(machine.ZIG_CACHE), game, probe)
    assert os.environ["ZIG_GLOBAL_CACHE_DIR"] == os.environ["ZIG_LOCAL_CACHE_DIR"] == str(other)
    assert other.is_dir()
    assert str(other) in capsys.readouterr().err
    assert not list(probe.iterdir())  # the probe's files are gone


def test_cache_stays_when_it_works(setup) -> None:
    game, probe, own, _, cc, calls = setup
    machine.settle_zig_cache(cc("nothing"), game, probe)
    assert os.environ["ZIG_GLOBAL_CACHE_DIR"] == str(own)
    assert len(calls.read_text()) == 1  # one probe compile


def test_cache_is_restored_when_no_folder_works(setup, capsys: pytest.CaptureFixture[str]) -> None:
    game, probe, own, _, cc, _ = setup
    machine.settle_zig_cache(cc(f"{machine.ZIG_CACHE},mkw-zig-cache"), game, probe)
    assert os.environ["ZIG_GLOBAL_CACHE_DIR"] == os.environ["ZIG_LOCAL_CACHE_DIR"] == str(own)
    assert "nor" in capsys.readouterr().err


def test_the_players_own_cache_is_left_alone(setup, monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    game, probe, _, _, cc, calls = setup
    theirs = tmp_path / "players-cache"
    monkeypatch.setenv("ZIG_GLOBAL_CACHE_DIR", str(theirs))
    machine.settle_zig_cache(cc("players-cache"), game, probe)
    assert os.environ["ZIG_GLOBAL_CACHE_DIR"] == str(theirs)
    assert not calls.exists()  # not even probed


def test_another_failure_does_not_move_the_cache(setup) -> None:
    game, probe, own, _, _, _ = setup
    failing = [sys.executable, "-c", "import sys; sys.stderr.write('error: unknown type name'); sys.exit(1)"]
    machine.settle_zig_cache(failing, game, probe)
    assert os.environ["ZIG_GLOBAL_CACHE_DIR"] == str(own)
