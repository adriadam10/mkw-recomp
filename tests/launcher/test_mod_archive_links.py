"""Archive links must not be copied into a mod by following their targets.

Usage: uv run pytest tests/launcher/test_mod_archive_links.py
"""

import importlib.util
import os
import tarfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("mod_import", ROOT / "tools/mod/import.py")
mod_import = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod_import)


@pytest.mark.parametrize("target", ["missing", "."])
def test_archive_link_refused(tmp_path, target):
    probe = tmp_path / "probe"
    try:
        os.symlink(target, probe, target_is_directory=target == ".")
    except OSError:
        pytest.skip("symlinks unavailable")
    probe.unlink()
    archive, dst = tmp_path / "mod.tar", tmp_path / "out"
    dst.mkdir()
    with tarfile.open(archive, "w") as tar:
        link = tarfile.TarInfo("romfs/link")
        link.type, link.linkname = tarfile.SYMTYPE, target
        tar.addfile(link)
    assert mod_import.unpack(str(archive), str(dst)) == ("layout", "the archive holds links")
