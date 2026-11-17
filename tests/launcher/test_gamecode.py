"""The compile step's answer to a compiler crash (tools/prepare/gamecode.py compile_one), with a fake compiler: a crash
(ICE) is tried once more and its first lines go to the log, a plain error is not retried (#506: clang crashed on a
different chunk each time on an i7-13700KF). Seconds, no game.

Usage: uv run pytest tests/launcher/test_gamecode.py
"""

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/prepare"))
import gamecode  # noqa: E402

# A compiler that answers from a script of outcomes, one per call: "ice", "error" or "ok" (writes the object).
FAKE_CC = """
import sys
from pathlib import Path
calls = Path(sys.argv[1])
n = len(calls.read_text()) if calls.exists() else 0
calls.write_text("x" * (n + 1))
plan = sys.argv[2].split(",")
outcome = plan[min(n, len(plan) - 1)]
if outcome == "ice":
    sys.stderr.write("Stack dump:\\n0. Program arguments: clang\\nPLEASE submit a bug report\\nFILES TO THE BUG REPORT:\\n")
    sys.exit(1)
if outcome == "error":
    sys.stderr.write("main_0001.c:1:1: error: unknown type name 'uint6'\\n")
    sys.exit(1)
Path(sys.argv[sys.argv.index("-o") + 1]).write_bytes(b"obj")
"""


def compile_with(tmp_path: Path, plan: str) -> tuple[str, int, Path]:
    fake = tmp_path / "fakecc.py"
    fake.write_text(FAKE_CC)
    calls = tmp_path / "calls"
    src, out, stamp = tmp_path / "main_0001.c", tmp_path / "main_0001.o", tmp_path / "main_0001.key"
    src.write_text("int x;\n")
    err = gamecode.compile_one([sys.executable, str(fake), str(calls), plan], [], src, out, stamp, "k")
    return err, len(calls.read_text()), stamp


@pytest.mark.parametrize(
    ("plan", "ok", "runs"),
    [("ok", True, 1), ("ice,ok", True, 2), ("ice,ice", False, 2), ("error,ok", False, 1)],
)
def test_crash_retried_once(tmp_path: Path, capsys: pytest.CaptureFixture[str], plan: str, ok: bool, runs: int) -> None:
    err, n, stamp = compile_with(tmp_path, plan)
    assert n == runs
    assert (err == "") == ok
    assert stamp.is_file() == ok
    if plan.startswith("ice"):
        assert "Program arguments" in capsys.readouterr().err  # the crash's head, which the 300-character tail lost
