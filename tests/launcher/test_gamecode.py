"""The compile step's answer to a compiler crash (tools/prepare/gamecode.py compile_one), with a fake compiler: a crash
(ICE) is tried once more and its first lines go to the log, a plain error is not retried (#506: clang crashed on a
different chunk each time on an i7-13700KF). Seconds, no game.

Usage: uv run pytest tests/launcher/test_gamecode.py
"""

import subprocess
import sys
from pathlib import Path, PureWindowsPath

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


@pytest.mark.skipif(sys.platform == "win32", reason="a shell script stands in for the compiler")
def test_compiler_id_leaves_out_the_package_path_and_host(tmp_path: Path) -> None:
    """The AppImage mounts the package at a new path each start: with InstalledDir in the key, every update on Linux
    compiled every chunk again."""
    ids = []
    for where, kernel in (("a", "linux7.3.4-gnu2.44.0"), ("b", "linux7.4.0-gnu2.44.0")):
        cc = tmp_path / where / "zig"
        cc.parent.mkdir()
        cc.write_text(f"#!/bin/sh\necho 'clang version 22.1.8'\necho 'Target: x86_64-unknown-{kernel}'\n"
                      f"echo 'InstalledDir: {cc.parent}'\n")  # fmt: skip
        cc.chmod(0o755)
        ids.append(gamecode.compiler_id([str(cc), "cc", "-target", "x86_64-linux-gnu"]))
    assert ids[0] == ids[1]
    assert "22.1.8" in ids[0] and "x86_64-linux-gnu" in ids[0]


@pytest.mark.skipif(sys.platform == "win32", reason="a shell script stands in for the compiler")
def test_legacy_compiler_is_the_played_folders_whole_version(tmp_path: Path) -> None:
    """An update run from the new package apart (prepare.py --next) still finds the key 0.1.13 gave the played folder's
    objects: the whole `--version` of the same zig, InstalledDir the game folder's setup/zig."""
    out = {}
    for where in ("game/setup/zig", "new/setup/zig"):
        cc = tmp_path / where / "zig"
        cc.parent.mkdir(parents=True)
        cc.write_text(f"#!/bin/sh\necho 'clang version 22.1.8'\necho 'InstalledDir: {cc.parent}'\n")
        cc.chmod(0o755)
        out[where] = subprocess.run([str(cc), "cc", "--version"], capture_output=True, text=True, check=True).stdout
    new = [str(tmp_path / "new/setup/zig/zig"), "cc"]
    assert gamecode.legacy_compilers(new, tmp_path / "game") == [out["game/setup/zig"]]
    assert gamecode.legacy_compilers(new, tmp_path / "elsewhere") == []


def test_installed_dir_forms_take_zigs_forward_slashes_on_windows() -> None:
    """zig on Windows prints InstalledDir as C:/upd/base/setup/zig: the backslash form alone never matched a 0.1.13
    object's key, and the first update compiled all ~360 chunks again (18/11/2026)."""
    forms = gamecode.installed_dirs(PureWindowsPath(r"C:\upd\base\setup\zig"))
    assert forms == {r"C:\upd\base\setup\zig", "C:/upd/base/setup/zig"}


def test_cpu_level(monkeypatch: pytest.MonkeyPatch) -> None:
    """MKW_SETUP_CPU picks the x86-64 level (CPUs without AVX2, discussion #82): v3 by default, v2 on request, and
    anything else stops the setup instead of building for a CPU nobody asked for."""
    monkeypatch.delenv("MKW_SETUP_CPU", raising=False)
    assert gamecode.cpu_level() == "v3"
    monkeypatch.setenv("MKW_SETUP_CPU", "v2")
    assert gamecode.cpu_level() == "v2"
    monkeypatch.setenv("MKW_SETUP_CPU", "v4")
    with pytest.raises(SystemExit):
        gamecode.cpu_level()
