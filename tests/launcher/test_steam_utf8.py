"""tools/prepare/steam.py writes its result line in UTF-8 whatever the Windows code page.
Why: the launcher runs it with -I (PYTHONIOENCODING ignored) and reads the pipe as UTF-8 (launcher_tools.cpp), and
"error io <detail>" names the Steam folder. On a cp1252 pipe a Steam library under D:/Spiele/Zoë came through as a
broken byte, and one under a CJK folder name crashed with UnicodeEncodeError inside the error handler, so Add to Steam
showed no reason at all. The Steam folder is stubbed.
Usage: uv run pytest tests/launcher/test_steam_utf8.py
"""

import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[2] / "tools/prepare"
DRIVER = """
import sys
from pathlib import Path
sys.stdout.reconfigure(encoding="cp1252")  # a legacy Windows pipe, as -I leaves it
tools, steam = sys.argv[1:3]
sys.path.insert(0, tools)
import steam as tool
tool.steam_dir = lambda: Path(steam)
sys.argv = ["steam.py", "add", "--name", "MKW", "--exe", "mkw.exe", "--dir", "."]
tool.main()
"""


@pytest.mark.parametrize("folder", ["Zoë", "田中"])
def test_an_io_error_naming_the_steam_folder_is_utf8(tmp_path: Path, folder: str) -> None:
    steam = tmp_path / folder / "Steam"
    (steam / "userdata/123").mkdir(parents=True)
    (steam / "userdata/123/config").write_bytes(b"")  # a file where the profile's folder goes: mkdir fails
    result = subprocess.run([sys.executable, "-I", "-c", DRIVER, str(TOOLS), str(steam)], capture_output=True, timeout=60)
    assert result.returncode == 1, result.stderr
    line = result.stdout.decode("utf-8").strip()
    assert line.startswith("error io ") and folder in line
