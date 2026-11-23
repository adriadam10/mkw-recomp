"""RenHDrum's host glue (runtime/input/renhdrum_host_win.c, with the vendored bridge renhdrum_recomp_bridge_win.c): what a
player sees with MKW_RENHDRUM off, on without renhdrum.dll, with one of another version and with one of the right version
(a mock with the eight functions the bridge needs, t_renhdrum_dll.c). The game's calls stay harmless in each case and the
log says why RenHDrum is not used. No DualSense and no real renhdrum.dll here: the controller side is for whoever has one.
Windows only (the bridge loads a DLL with LoadLibraryExW); skipped elsewhere and without a C compiler.

Usage: uv run pytest tests/runtime/test_renhdrum.py
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests/runtime"
INPUT = ROOT / "runtime/input"
RIGHT_VERSION = "0x000500"  # RENHDRUM_VERSION of runtime/input/renhdrum.h: 0.5.0

pytestmark = pytest.mark.skipif(os.name != "nt", reason="the bridge is Windows only")


STUBS = {  # what renhdrum_host_win.c includes of the runtime, cut to the functions it calls (t_renhdrum.c defines them)
    "cfg.h": "#pragma once\nint cfg_bool(const char *name);\n",
    "host.h": "#pragma once\nvoid rt_log(const char *fmt, ...);\n",
    "platform/platform.h": (
        "#pragma once\n#include <stdint.h>\nuint64_t plat_now_ns(void);\nint32_t plat_xchg32(volatile int32_t *p, int32_t v);\n"
    ),
    "SDL3/SDL.h": "#pragma once\n#include <stdbool.h>\n#include <stdint.h>\nbool SDL_InitSubSystem(uint32_t flags);\n",
}


def write_stubs(folder: Path) -> Path:
    for name, text in STUBS.items():
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    return folder


def build(tmp_path: Path, dll_version: str | None) -> Path:
    """The test program in its own folder, with a mock renhdrum.dll of that version beside it (none: no DLL)."""
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        pytest.skip("no C compiler")
    folder = tmp_path / ("dll" if dll_version else "nodll")
    folder.mkdir()
    stubs = write_stubs(tmp_path / ("stubs-dll" if dll_version else "stubs-nodll"))
    exe = folder / "t_renhdrum.exe"
    subprocess.run(
        [cc, "-O1", f"-I{stubs}", f"-I{INPUT}", str(TESTS / "t_renhdrum.c"),
         str(INPUT / "renhdrum_host_win.c"), str(INPUT / "renhdrum_recomp_bridge_win.c"), "-o", str(exe)],
        check=True,
    )  # fmt: skip
    if dll_version:
        subprocess.run(
            [cc, "-shared", f"-DMOCK_VERSION={dll_version}", str(TESTS / "t_renhdrum_dll.c"), "-o", str(folder / "renhdrum.dll")],
            check=True,
        )  # fmt: skip
    return exe


def run(exe: Path, setting: str) -> str:
    env = {**os.environ, "MKW_RENHDRUM": setting}
    env.pop("HOS_RENHDRUM", None)
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=False, env=env, timeout=30)
    assert out.returncode == 0, out.stdout + out.stderr
    assert out.stdout.rstrip().endswith("done"), out.stdout
    return out.stdout


def test_off_by_default_says_nothing_and_claims_nothing(tmp_path: Path) -> None:
    out = run(build(tmp_path, RIGHT_VERSION), "0")
    assert "renhdrum" not in out.replace("claims", "")
    assert "claims=0" in out


def test_without_the_dll_the_log_says_why_and_the_gamepad_keeps_its_motors(tmp_path: Path) -> None:
    out = run(build(tmp_path, None), "1")
    assert "renhdrum: not used, normal rumble (renhdrum.dll not found beside exe: normal rumble)" in out
    assert "claims=0" in out


def test_a_dll_of_another_version_is_refused(tmp_path: Path) -> None:
    out = run(build(tmp_path, "0x000300"), "1")
    assert "Incompatible renhdrum.dll (0x000300" in out
    assert "claims=0" in out


def test_the_right_dll_loads_and_every_call_is_harmless_without_a_controller(tmp_path: Path) -> None:
    out = run(build(tmp_path, RIGHT_VERSION), "1")
    assert "renhdrum: renhdrum.dll loaded (MKW_RENHDRUM=1)" in out
    assert "claims=0" in out
