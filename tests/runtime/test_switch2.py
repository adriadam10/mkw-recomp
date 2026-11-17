"""The Switch 2 controllers' Bluetooth LE protocol (runtime/input/switch2_proto.h, from mkw-neo): mkw-neo's own test
(tests/runtime/t_switch2.c, reports and replies recorded from a left and a right Joy-Con 2) built with the C compiler at hand
against our copy, whose DM_* names take our button bits. We own no such controller: these recordings are the check.

Usage: uv run pytest tests
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_switch2_protocol(tmp_path: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    exe = tmp_path / "t_switch2.exe"
    libm = [] if os.name == "nt" else ["-lm"]
    subprocess.run([cc, "-O1", f"-I{ROOT / 'runtime/input'}", str(ROOT / "tests/runtime/t_switch2.c"), "-o", str(exe), *libm],
                   check=True)  # fmt: skip
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
    assert out.returncode == 0 and " 0 failed" in out.stdout, out.stdout
