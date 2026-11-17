"""The actual audren RequestUpdate path (tests/runtime/t_audio_update.c) fed synthetic REV11/REV15 voice records.

Why: unused client slots can be zeroed, including their ID; they must not reset active voice 0. The harness uses the
runtime's wire layouts and update parser, with only mixer/platform calls stubbed. No game or audio device required.
Usage: uv run --with ziglang pytest tests/runtime/test_audio_update.py (ziglang is optional with clang/cc/gcc installed).
"""

import importlib.util
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]

@pytest.fixture(scope="module")
def renderer_update(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    command = [cc] if cc else []
    if not command:
        spec = importlib.util.find_spec("ziglang")
        zig = shutil.which("zig")
        if not zig and spec and spec.origin:
            zig = str(Path(spec.origin).parent / ("zig.exe" if os.name == "nt" else "zig"))
        if not zig:
            pytest.skip("no C compiler; use uv run --with ziglang pytest tests/runtime/test_audio_update.py")
        command = [zig, "cc"]
    src = (ROOT / "runtime/audio/audren.c").read_text(encoding="utf-8")
    assert "// ---- RequestUpdate" in src and "// ---- render tick:" in src, "audren.c changed: update test_audio_update.py"
    begin = src.index("// ---- RequestUpdate")
    end = src.index("// ---- render tick:")
    d = tmp_path_factory.mktemp("audren-update")
    (d / "request.inc").write_text(src[begin:end], encoding="utf-8")
    includes = [f"-I{ROOT / 'runtime' / folder}" for folder in ("audio", "core/base", "cpu", "platform")]
    includes += [f"-I{ROOT / 'runtime'}", f"-I{d}"]
    exe = d / "update.exe"
    subprocess.run([*command, "-O1", *includes, str(ROOT / "tests/runtime/t_audio_update.c"), "-o", str(exe)], check=True)
    return exe


@pytest.mark.parametrize("revision", [11, 15])
def test_unused_voice_records_cannot_reset_voice_zero(renderer_update: Path, revision: int) -> None:
    out = subprocess.run([str(renderer_update), str(revision)], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout + out.stderr
