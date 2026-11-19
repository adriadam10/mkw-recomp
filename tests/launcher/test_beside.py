"""prepare.py's steps run beside the earlier ones (tools/prepare/beside.py): progress held until shown, errors raised
in the thread that joins."""

import sys
import threading
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/prepare"))
from beside import Beside
from machine import Failed


def test_progress_is_held_until_shown(capsys: pytest.CaptureFixture[str]) -> None:
    go = threading.Event()

    def job(b: Beside) -> None:
        b.progress("progress 1 3")
        b.progress("progress 2 3")
        go.wait()
        b.progress("progress 3 3")

    b = Beside("shaders", job)
    while b._held != "progress 2 3":  # noqa: SLF001
        pass
    assert capsys.readouterr().out == ""  # the start screen still draws the step before
    go.set()
    b.join(show=True)
    out = capsys.readouterr().out.splitlines()
    assert out[0] in ("progress 2 3", "progress 3 3") and out[-1] == "progress 3 3"


def test_an_error_is_raised_where_it_is_joined() -> None:
    def job(_: Beside) -> None:
        raise Failed("translate.py failed", "failed")

    b = Beside("shaders", job)
    with pytest.raises(Failed, match="translate.py"):
        b.join()
