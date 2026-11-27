"""tools/prepare/update.py clean --previous keeps the replaced files even when update-previous is still there.
Why: the clean-up empties update-previous with shutil.rmtree(ignore_errors=True), and a file Windows still holds (a
running exe kept there) stays behind. The folder is then not new, and keep.mkdir() without exist_ok raised
FileExistsError: the clean-up stopped with a traceback instead of moving the replaced files into the folder.
Usage: uv run pytest tests/launcher/test_update_clean.py
"""

import argparse
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[2] / "tools/prepare"
sys.path.insert(0, str(TOOLS))

import update  # noqa: E402


@pytest.fixture
def game(tmp_path: Path) -> Path:
    folder = tmp_path / "mkw-recomp"
    folder.mkdir()
    (folder / "mkw.exe.old").write_bytes(b"MZ new-replaced")
    return folder


def test_previous_folder_left_behind_by_a_locked_file_does_not_stop_the_clean_up(game, monkeypatch):
    leftover = game / update.PREVIOUS
    leftover.mkdir()
    (leftover / "locked.exe").write_bytes(b"MZ still running")
    monkeypatch.setattr(update.shutil, "rmtree", lambda *args, **kwargs: None)

    update.clean(argparse.Namespace(game=game, previous="v0.1.17"))

    assert (leftover / "mkw.exe").read_bytes() == b"MZ new-replaced"
    assert (leftover / "version").read_text(encoding="utf-8") == "v0.1.17\n"
    assert not (game / "mkw.exe.old").exists()
