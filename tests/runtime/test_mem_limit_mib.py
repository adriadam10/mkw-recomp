"""Check how MKW_GPU_MEM_LIMIT and MKW_VRAM_MARGIN are parsed. Usage: uv run pytest tests/runtime/test_mem_limit_mib.py.

Why: vk_device.c read both with atoll, so "abc" lifted the memory cap (0) and "-300" wrapped the VRAM margin past
every heap budget. mib_setting (vk_device.c) must accept whole MiB only and report anything else.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

SOURCE = Path(__file__).resolve().parents[2] / "runtime/gfx/vk/device/vk_device.c"

HARNESS = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

static int logged;
static void rt_log(const char *format, ...) { (void)format; logged++; }

#include "mib.inc"

int main(void) {
    uint64_t m = 7;
    assert(mib_setting("K", "4096", &m) && m == 4096 && !logged);
    assert(mib_setting("K", "0", &m) && m == 0 && !logged);
    assert(mib_setting("K", "300", NULL) && !logged);
    m = 7;
    assert(!mib_setting("K", "abc", &m) && m == 7 && logged == 1);
    assert(!mib_setting("K", "-300", &m) && m == 7 && logged == 2);
    assert(!mib_setting("K", "", &m) && m == 7 && logged == 3);
    assert(!mib_setting("K", "6g", &m) && m == 7 && logged == 4);
    assert(!mib_setting("K", "99999999999999999999", &m) && m == 7 && logged == 5);
    return 0;
}
"""


def test_mem_limit_settings_reject_what_atoll_misread(tmp_path: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static int mib_setting(")
    end = source.index("\n}", start) + len("\n}")
    (tmp_path / "mib.inc").write_text(source[start:end], encoding="utf-8")
    harness = tmp_path / "main.c"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = tmp_path / "mem_limit_mib.exe"
    subprocess.run([cc, "-std=c11", "-Wall", "-Werror", str(harness), "-o", str(executable)], check=True, timeout=30)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
