"""The Linux runtime finds the lib/ beside it (tools/build/build.py LINUX.link_flags): issues #825, #839 and #840 were
"closed with code 127 after 0 s: libvpx.so.6: cannot open shared object file" from a launcher that unpacks the AppImage and
runs mkw itself, on a distro without that old libvpx; only the AppRun sets LD_LIBRARY_PATH to lib/. The flag goes through
ninja and the shell on its way to the linker, so the second test builds a program with the flag as build.py spells it, with
ninja, and runs it from another folder without LD_LIBRARY_PATH. Linux only, needs a C compiler and ninja.

Usage: uv run pytest tests/runtime/test_linux_rpath.py
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/build"))
import build  # noqa: E402


def rpath_flag() -> str:
    flags = [f for f in build.LINUX.link_flags if "-rpath" in f]
    assert len(flags) == 1, build.LINUX.link_flags
    return flags[0]


def test_the_linux_link_flags_name_the_lib_folder_beside_the_exe() -> None:
    assert rpath_flag() == "'-Wl,-rpath,$$ORIGIN/lib'"  # $$ for ninja, the quotes keep $ORIGIN from the shell


@pytest.mark.skipif(sys.platform != "linux", reason="a Linux loader")
def test_the_exe_finds_its_lib_folder_without_ld_library_path(tmp_path: Path) -> None:
    cc, ninja = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang"), shutil.which("ninja")
    if not cc or not ninja:
        pytest.skip("no C compiler or ninja")
    (tmp_path / "lib").mkdir()
    (tmp_path / "foo.c").write_text("int foo(void) { return 42; }\n")
    (tmp_path / "main.c").write_text(
        '#include <stdio.h>\nint foo(void);\nint main(void) { printf("%d\\n", foo()); return 0; }\n'
    )
    # the flags as build.py writes them to build.ninja: ninja unescapes $$, the shell removes the quotes
    (tmp_path / "build.ninja").write_text(
        f"ldflags = {rpath_flag()}\n"
        f"rule so\n  command = {cc} -shared -fPIC $in -o $out\n"
        f"rule exe\n  command = {cc} $in -o $out -Llib -lfoo $ldflags\n"
        "build lib/libfoo.so: so foo.c\n"
        "build mkw: exe main.c | lib/libfoo.so\n"
    )  # fmt: skip
    subprocess.run([ninja, "-C", str(tmp_path)], check=True, capture_output=True)
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    elsewhere = tmp_path.parent
    out = subprocess.run([str(tmp_path / "mkw")], cwd=elsewhere, env=env, capture_output=True, text=True, check=False)
    assert out.returncode == 0 and out.stdout.strip() == "42", out.stderr
    # and the folder is what finds it: move lib/ away and the loader fails with the error of the issues
    (tmp_path / "lib").rename(tmp_path / "lib.away")
    out = subprocess.run([str(tmp_path / "mkw")], cwd=elsewhere, env=env, capture_output=True, text=True, check=False)
    assert out.returncode != 0 and "libfoo.so" in out.stderr
