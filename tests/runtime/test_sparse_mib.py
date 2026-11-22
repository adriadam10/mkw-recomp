"""Check how MKW_SPARSE_MIB (sparse_mib) is parsed. Usage: uv run pytest tests/runtime/test_sparse_mib.py.

Why: the budget took the value with atol, so a typo ("abc", "1g") or a negative number lifted or wrapped the cap on
sparse tile memory instead of keeping the GPU's default. parse_mib (vk_sparse.c) must accept only whole MiB.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

SOURCE = Path(__file__).resolve().parents[2] / "runtime/gfx/vk/tex/vk_sparse.c"

HARNESS = r"""
#include <assert.h>
#include <stdint.h>

#include "parse.inc"

int main(void) {
    uint64_t m = 7;
    assert(parse_mib("512", &m) && m == 512);
    assert(parse_mib("0", &m) && m == 0);
    m = 7;
    assert(!parse_mib("", &m) && m == 7);
    assert(!parse_mib("abc", &m) && m == 7);
    assert(!parse_mib("1g", &m) && m == 7);
    assert(!parse_mib("-256", &m) && m == 7);
    assert(!parse_mib("512.5", &m) && m == 7);
    assert(!parse_mib("99999999999999999999", &m) && m == 7);
    return 0;
}
"""


def test_sparse_mib_rejects_what_atol_misread(tmp_path: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static int parse_mib(")
    end = source.index("\n}", start) + len("\n}")
    (tmp_path / "parse.inc").write_text(source[start:end], encoding="utf-8")
    harness = tmp_path / "main.c"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = tmp_path / "sparse_mib.exe"
    subprocess.run([cc, "-std=c11", "-Wall", "-Werror", str(harness), "-o", str(executable)], check=True, timeout=30)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
