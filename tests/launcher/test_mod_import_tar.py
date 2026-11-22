"""tools/mod/import.py reads rar, 7z and tar mods with bsdtar, on Windows the one in System32 (tar.exe).
Why: it took the first "tar" on PATH. With Git for Windows' or MSYS2's usr/bin there first, that is GNU tar, which
reads "C:\\...\\mod.rar" as a remote host "C" and answered "Cannot connect to C: resolve failed" for every archive (and
reads no rar or 7z at all). The lookup is checked with a GNU-style tar first on PATH.
Usage: uv run pytest tests/launcher/test_mod_import_tar.py
"""

import importlib.util
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("mod_import", ROOT / "tools/mod/import.py")
mod_import = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod_import)
SYSTEM_TAR = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/tar.exe"


@pytest.mark.skipif(sys.platform != "win32" or not SYSTEM_TAR.is_file(), reason="Windows' own tar.exe")
def test_windows_takes_its_own_bsdtar_over_another_tar_on_path(tmp_path: Path, monkeypatch) -> None:
    (tmp_path / "tar.exe").write_bytes(b"MZ")  # GNU tar from Git or MSYS2, first on PATH
    monkeypatch.setenv("PATH", f"{tmp_path}{os.pathsep}{os.environ['PATH']}")
    assert Path(mod_import.tar_tool()).resolve() == SYSTEM_TAR.resolve()


def test_bsdtar_on_path_wins(tmp_path: Path, monkeypatch) -> None:
    name = "bsdtar.exe" if sys.platform == "win32" else "bsdtar"
    fake = tmp_path / name
    fake.write_bytes(b"")
    fake.chmod(0o755)
    monkeypatch.setenv("PATH", str(tmp_path))
    assert Path(mod_import.tar_tool()) == fake
