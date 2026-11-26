"""Guest pages of 4 KiB on larger host pages (runtime/platform/linux_mem.c, built alone with t_host_pages.c): a guest
protection on part of a host page never fails and never takes access from a neighbour, a decommitted guest page reads
zero when committed again, and the write watch loses no written page. Run with the kernel's pages and with
MKW_HOST_PAGE_KIB=16 and 64, which take host pages as that large on a 4 KiB kernel (Linux on Apple silicon has 16 KiB
pages: 0.1.18 died there at subsdk0's data permission). On a 4 KiB kernel the kernel's write marks stay per 4 KiB, so
one pool's reset taking a neighbour's marks in a shared host page is only tested on a kernel with larger pages: run it
there too. Linux only.

Usage: uv run pytest tests/runtime/test_host_pages.py
"""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CC = shutil.which("clang") or shutil.which("cc")


@pytest.fixture(scope="module")
def exe(tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("host_pages") / "t_host_pages"
    plat = ROOT / "runtime/platform"
    subprocess.run(
        [CC, "-O1", "-Wall", "-Werror", "-I", str(plat), "-I", str(ROOT / "runtime"),
         str(plat / "linux_mem.c"), str(plat / "linux_share.c"), str(ROOT / "tests/runtime/t_host_pages.c"),
         "-o", str(out)],
        check=True,
    )  # fmt: skip
    return out


@pytest.mark.skipif(not sys.platform.startswith("linux") or not CC, reason="Linux only, needs a C compiler")
@pytest.mark.parametrize("kib", ["", "16", "64"])
def test_host_pages(exe: Path, kib: str) -> None:
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env={"MKW_HOST_PAGE_KIB": kib})
    assert r.returncode == 0, r.stdout + r.stderr
    assert r.stdout.strip().endswith("ok")
    if kib:
        assert f"host pages: {kib} KiB" in r.stdout


@pytest.mark.skipif(not sys.platform.startswith("linux") or not CC, reason="Linux only, needs a C compiler")
def test_bad_page_size(exe: Path) -> None:
    """A value that is not a power of two from 4 to 64 KiB warns and keeps the kernel's size."""
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env={"MKW_HOST_PAGE_KIB": "12"})
    assert r.returncode == 0, r.stdout + r.stderr
    assert "MKW_HOST_PAGE_KIB=12" in r.stderr and "ignored" in r.stderr
