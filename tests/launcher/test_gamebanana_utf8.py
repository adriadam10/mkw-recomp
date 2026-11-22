"""tools/mod/gamebanana.py writes its event lines in UTF-8 whatever the Windows code page.
Why: the launcher runs it with -I (PYTHONIOENCODING ignored) and reads the pipe as UTF-8 (mods_browse.cpp). On a
cp1252 pipe a mod named "Café" reached the Mods page as a broken byte, and one with CJK or emoji in its name or
description failed the whole page with "error net 'charmap' codec can't encode". The network answer is stubbed.
Usage: uv run pytest tests/launcher/test_gamebanana_utf8.py
"""

import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[2] / "tools/mod"
DRIVER = """
import sys
sys.stdout.reconfigure(encoding="cp1252")  # a legacy Windows pipe, as -I leaves it
tools, name, cache = sys.argv[1:4]
sys.path.insert(0, tools)
import gamebanana
gamebanana.info = lambda cache, mod_id: {
    "id": mod_id, "name": name, "author": name, "license": "", "url": "u", "version": "1",
    "vis": "show", "ratings": "", "text": name, "files": [], "images": [],
}
sys.argv = ["gamebanana.py", "--cache", cache, "info", "7"]
sys.exit(gamebanana.main())
"""


@pytest.mark.parametrize("name", ["Café", "マリオ", "Kart 🏎"])
def test_info_lines_are_utf8_on_a_legacy_pipe(tmp_path: Path, name: str) -> None:
    args = [sys.executable, "-I", "-c", DRIVER, str(TOOLS), name, str(tmp_path / "cache")]
    result = subprocess.run(args, capture_output=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    lines = result.stdout.decode("utf-8").splitlines()
    assert lines[0].split("\t")[:3] == ["info", "7", name]
    assert lines[1] == f"text\t{name}"
