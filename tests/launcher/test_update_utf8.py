"""tools/prepare/update.py writes its event lines in UTF-8 whatever the Windows code page.
Why: the launcher runs it with -I (PYTHONIOENCODING ignored) and reads the pipe as UTF-8 (launcher_update.cpp), and
"ready <path>" names the downloaded package in the game folder. On a cp1252 pipe a game folder under C:/Users/Zoë came
through as a broken byte, and one under a CJK user name crashed the fetch with UnicodeEncodeError after the download,
so the update never got its path. The release answer and the download are stubbed.
Usage: uv run pytest tests/launcher/test_update_utf8.py
"""

import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[2] / "tools/prepare"
DRIVER = """
import contextlib, hashlib, io, sys, zipfile
sys.stdout.reconfigure(encoding="cp1252")  # a legacy Windows pipe, as -I leaves it
tools, dest = sys.argv[1:3]
sys.path.insert(0, tools)
import update
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w") as z:
    z.writestr("mkw-recomp/mkw.exe", b"MZ")
data = buf.getvalue()
asset = {"name": "mkw-recomp-windows-x64.zip", "size": len(data), "browser_download_url": "u",
         "digest": "sha256:" + hashlib.sha256(data).hexdigest()}
update.latest_asset = lambda os_name: ("v9.9.9", asset)
update.get = lambda url, timeout: contextlib.closing(io.BytesIO(data))
sys.argv = ["update.py", "fetch", "--tag", "v9.9.9", "--dest", dest, "--os", "windows"]
update.main()
"""


@pytest.mark.parametrize("user", ["Zoë", "田中"])
def test_ready_names_the_package_in_utf8_on_a_legacy_pipe(tmp_path: Path, user: str) -> None:
    dest = tmp_path / user / "update"
    result = subprocess.run([sys.executable, "-I", "-c", DRIVER, str(TOOLS), str(dest)], capture_output=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    lines = result.stdout.decode("utf-8").splitlines()
    assert lines[-1] == f"ready {dest.resolve() / 'new' / 'mkw-recomp'}"
