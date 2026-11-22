"""tools/mod/merge.py writes its event lines in UTF-8 whatever the Windows code page.
Why: the launcher runs it with -I (PYTHONIOENCODING ignored) and reads the pipe as UTF-8 (mods.cpp). On a cp1252 pipe
a game folder under C:/Users/Zoë reached the Mods page as a broken byte, and one under a CJK user name crashed the
script with UnicodeEncodeError before its "error" line, so the page had no reason to show.
Usage: uv run pytest tests/launcher/test_mod_merge_utf8.py
"""

import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[2] / "tools/mod/merge.py"
DRIVER = (
    "import runpy,sys; sys.stdout.reconfigure(encoding='cp1252'); "  # a legacy Windows pipe, as -I leaves it
    "sys.argv=sys.argv[1:]; runpy.run_path(sys.argv[0],run_name='__main__')"
)


@pytest.mark.parametrize("user", ["Zoë", "田中"])
def test_an_error_naming_the_game_folder_is_utf8(tmp_path: Path, user: str) -> None:
    game = tmp_path / user / "mkw"
    (game / "mods/A").mkdir(parents=True)
    romfs = game / "romfs"
    args = [sys.executable, "-I", "-c", DRIVER, str(SCRIPT), "--game", str(game), "--romfs", str(romfs),
            "--enable", "A,A"]  # fmt: skip
    result = subprocess.run(args, capture_output=True, timeout=60)
    assert result.returncode == 1, result.stderr
    assert result.stdout.decode("utf-8").strip() == f"error romfs {romfs} is not the game's RomFS (a folder or a .nup)"
