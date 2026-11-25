"""Check plat_mem_readable on ranges that reach 2^64. Usage: uv run pytest tests/runtime/test_mem_readable.py.

Why: the frame walks (runtime/core/boot/main.c, runtime/core/kernel/svc.c) ask it for 16 bytes at a frame pointer
read from guest memory. Linux computed addr + n, which wraps past 2^64, so its loop did not run and it said yes to
0xfffffffffffffff8: the walk read there and faulted. Windows' first lookup succeeds on a committed address and the
wrapped end then said yes to a huge n. Both are cut out of runtime/platform/{linux,win32}_mem.c and built here on any
host, with the memory scripted: [0x10000, 0x30000) readable, the rest not, and nothing at or above 2^47.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

PLATFORM = Path(__file__).resolve().parents[2] / "runtime/platform"

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LO = 0x10000, HI = 0x30000 };
#define TOP 0x800000000000ull   // the user address space's end

// linux_mem.c: plat_mem_read copies n bytes or returns 0
static int plat_mem_read(uint64_t addr, void *out, size_t n) {
    if (addr < LO || addr + n > HI) return 0;
    memset(out, 0, n);
    return 1;
}

// win32_mem.c: VirtualQuery, its region at a: the readable one, or free memory up to the next region
typedef size_t SIZE_T;
typedef uint32_t DWORD;
typedef struct { void *BaseAddress; SIZE_T RegionSize; DWORD State, Protect; } MEMORY_BASIC_INFORMATION;
enum { MEM_COMMIT = 0x1000, MEM_FREE = 0x10000, PAGE_NOACCESS = 0x01, PAGE_READWRITE = 0x04, PAGE_GUARD = 0x100 };
static SIZE_T VirtualQuery(const void *p, MEMORY_BASIC_INFORMATION *m, SIZE_T size) {
    uint64_t a = (uint64_t)(uintptr_t)p & ~0xfffull;
    (void)size;
    if (a >= TOP) return 0;
    if (a >= LO && a < HI) *m = (MEMORY_BASIC_INFORMATION){(void *)(uintptr_t)LO, HI - LO, MEM_COMMIT, PAGE_READWRITE};
    else *m = (MEMORY_BASIC_INFORMATION){(void *)(uintptr_t)a, (a < LO ? LO : TOP) - a, MEM_FREE, PAGE_NOACCESS};
    return sizeof *m;
}

#include "readable.inc"

int main(void) {
    static const struct { uint64_t addr, n; int want; const char *what; } cases[] = {
        {LO, HI - LO, 1, "the whole readable range"},
        {LO, HI - LO + 1, 0, "one byte past it"},
        {HI - 1, 0, 1, "n 0: the byte at addr"},
        {HI, 0, 0, "n 0 past the end"},
        {0xfffffffffffffff8ull, 16, 0, "a frame pointer 8 bytes below 2^64 (the frame walks' 16 bytes)"},
        {~0ull - 0x10, 0x100, 0, "a range across 2^64"},
        {~0ull, 0, 0, "n 0 at the last byte"},
        {LO, ~0ull, 0, "a readable start and an n that wraps"},
        {LO, 0 - (uint64_t)LO, 0, "a readable start, ending at 2^64 exactly"},
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        int l = linux_readable(cases[i].addr, cases[i].n), w = win32_readable(cases[i].addr, cases[i].n);
        if (l != cases[i].want || w != cases[i].want)
            printf("FAIL: %s: Linux %d, Windows %d, want %d\n", cases[i].what, l, w, cases[i].want), bad++;
    }
    return bad != 0;
}
"""


def cut(name: str, rename: str) -> str:
    source = (PLATFORM / name).read_text(encoding="utf-8")
    start = source.index("int plat_mem_readable(")
    end = source.index("\n}\n", start) + len("\n}\n")
    return re.sub(r"^int plat_mem_readable\(", f"static int {rename}(", source[start:end])


def test_mem_readable_past_2_64(tmp_path: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    inc = cut("linux_mem.c", "linux_readable") + cut("win32_mem.c", "win32_readable")
    (tmp_path / "readable.inc").write_text(inc, encoding="utf-8")
    harness = tmp_path / "main.c"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = tmp_path / "mem_readable.exe"
    subprocess.run([cc, "-std=c11", "-Wall", "-Werror", str(harness), "-o", str(executable)], check=True, timeout=30)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
