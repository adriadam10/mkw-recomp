"""Run lazy Vulkan pool backing with synthetic allocation results. Usage: uv run pytest tests/runtime/test_lazy_pool.py."""

import shutil
import subprocess
from pathlib import Path

import pytest

SOURCE = Path(__file__).resolve().parents[2] / "runtime/gfx/vk/mem/vk_mem_back.c"

HARNESS = r"""
#include <assert.h>
#include <stdint.h>

typedef struct {
    uint64_t lo, size;
    unsigned flags;
    int lazy, buf;
} PoolRec;

static int32_t n_import_failures;
static int import_ok;
static int32_t plat_inc32(int32_t *n) { return ++*n; }
static void mem_thaw(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
static void *vk_ctx(void) { return 0; }
static void rt_log(const char *format, ...) { (void)format; }

static int import_pool(void *ctx, PoolRec *r, uint64_t addr, uint64_t size) {
    (void)ctx;
    (void)addr;
    (void)size;
    if (import_ok) r->buf = 1;
    return import_ok;
}

#include "lazy.inc"

int main(void) {
    PoolRec failed = {.lo = 0x1000, .size = 0x4000, .flags = 0x22, .lazy = 1};
    lazy_back(&failed);
    assert(!failed.lazy && !failed.buf);
    assert(n_import_failures == 1);

    PoolRec backed = {.lo = 0x8000, .size = 0x4000, .flags = 0x22, .lazy = 1};
    import_ok = 1;
    lazy_back(&backed);
    assert(!backed.lazy && backed.buf);
    assert(n_import_failures == 1);
    return 0;
}
"""


def test_lazy_pool_backing_counts_failures(tmp_path: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static long n_lazy_backed;")
    end = source.index("\n}", start) + len("\n}")
    (tmp_path / "lazy.inc").write_text(source[start:end], encoding="utf-8")
    harness = tmp_path / "main.c"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = tmp_path / "lazy_pool.exe"
    subprocess.run([cc, "-std=c11", str(harness), "-o", str(executable)], check=True, timeout=30)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
