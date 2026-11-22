"""Check the breadcrumb report's idle test on device-loss marker words. Usage: uv run pytest tests/runtime/test_crumbs_idle.py.

Why: issue #557's GTX 1080 log lost the device with the upload ring at 5139/5139 and the replay ring at 4431/4431
(started/finished); the report named finished work as the hang. crumbs_idle (vk_crumbs.c) must say no marked work ran.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

SOURCE = Path(__file__).resolve().parents[2] / "runtime/gfx/vk/diag/vk_crumbs.c"

HARNESS = r"""
#include <assert.h>
#include <stdint.h>

#include "idle.inc"

int main(void) {
    volatile uint32_t issue557[4] = {5139, 5139, 4431, 4431};
    assert(crumbs_idle(issue557));
    volatile uint32_t replay_busy[4] = {5139, 5139, 4432, 4431};
    assert(!crumbs_idle(replay_busy));
    volatile uint32_t upload_busy[4] = {12, 10, 7, 7};
    assert(!crumbs_idle(upload_busy));
    volatile uint32_t no_marks[4] = {0, 0, 0, 0};
    assert(!crumbs_idle(no_marks));
    volatile uint32_t replay_only[4] = {0, 0, 9, 9};
    assert(crumbs_idle(replay_only));
    return 0;
}
"""


def test_crumbs_idle_on_issue_557_words(tmp_path: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static int crumbs_idle(")
    end = source.index("\n", start) + 1
    (tmp_path / "idle.inc").write_text(source[start:end], encoding="utf-8")
    harness = tmp_path / "main.c"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = tmp_path / "crumbs_idle.exe"
    subprocess.run([cc, "-std=c11", str(harness), "-o", str(executable)], check=True, timeout=30)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
