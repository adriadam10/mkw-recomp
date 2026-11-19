"""A mod's settings per player (runtime/plugin/mod_settings.c), the store behind the plugin API's setting_get /
setting_set and the options page's widgets: built alone with t_mod_settings.c, which stubs the platform layer's file
and lock calls on stdio.

Usage: uv run pytest tests/runtime/test_mod_settings.py
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_mod_settings(tmp_path: Path) -> None:
    if os.name == "nt":
        pytest.skip("the stubs use POSIX mkdir")
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    exe = tmp_path / "t_mod_settings"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'runtime'}",
                    f"-I{ROOT / 'runtime/plugin'}", f"-I{ROOT / 'runtime/platform'}",
                    str(ROOT / "tests/runtime/t_mod_settings.c"), str(ROOT / "runtime/plugin/mod_settings.c"),
                    "-o", str(exe)], check=True)
    work = tmp_path / "players"
    work.mkdir()
    out = subprocess.run([str(exe), str(work)], capture_output=True, text=True)
    assert out.returncode == 0 and out.stdout.strip() == "ok", out.stdout + out.stderr
