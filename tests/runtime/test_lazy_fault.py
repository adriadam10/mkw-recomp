"""Check Windows' lazy commit hook on every page state. Usage: uv run pytest tests/runtime/test_lazy_fault.py.

Why: two threads that touch the same reserved 2 MiB of the heap both fault; the first commits it (lazy_fault,
runtime/core/memory/memory.c), and the second can find the page committed by the time its hook looks. A write then
goes on to thaw_fault, which retries it, but a read has no hook after this one (win32_mem.c fault_veh): it went to the
crash handler. lazy_fault is cut out of memory.c and built alone with the page state scripted.
"""

import shutil
import subprocess
from pathlib import Path

import pytest

SOURCE = Path(__file__).resolve().parents[2] / "runtime/core/memory/memory.c"

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>

#define PAGE 0x1000ull
#define LAZY_CHUNK (2ull << 20)
enum { PLAT_PROT_NONE, PLAT_PROT_READ, PLAT_PROT_RW };
enum { PLAT_MEM_FREE = -2, PLAT_MEM_RESERVED = -1 };
typedef int PlatMutex;

static int lazy = 1, states[2], nstate, commits;   // plat_mem_state's answers: at the first look, after the commit
static PlatMutex lock;
static int watched(uint64_t a, uint64_t n) { (void)a, (void)n; return 1; }
static void plat_mutex_lock(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static void owned_commit_locked(uint64_t a, uint64_t n) { (void)a, (void)n; commits++; }
static int plat_mem_state(uint64_t a, uint64_t limit, uint64_t *end) {
    (void)a;
    *end = limit;
    return states[nstate++ ? 1 : 0];
}

#include "lazy_fault.inc"

int main(void) {
    static const struct { int write, first, after, want, commits; const char *what; } cases[] = {
        {0, PLAT_PROT_RW, 0, 1, 0, "a read of a page another thread committed after the fault retries"},
        {0, PLAT_PROT_READ, 0, 1, 0, "a read of a page another thread committed and the back end froze again retries"},
        {0, PLAT_PROT_NONE, 0, 0, 0, "a read of an inaccessible committed page (a guard) goes on to the crash handler"},
        {1, PLAT_PROT_RW, 0, 0, 0, "a write of a committed page goes on to thaw_fault, which retries a read-write one"},
        {1, PLAT_PROT_READ, 0, 0, 0, "a write of a read-only page goes on to thaw_fault, which thaws a frozen region"},
        {0, PLAT_MEM_RESERVED, PLAT_PROT_RW, 1, 1, "a read of a reserved page commits its 2 MiB and retries"},
        {1, PLAT_MEM_RESERVED, PLAT_PROT_RW, 1, 1, "a write of a reserved page commits its 2 MiB and retries"},
        {0, PLAT_MEM_RESERVED, PLAT_MEM_RESERVED, 0, 1, "a reserved page the game does not own goes on"},
        {0, PLAT_MEM_FREE, 0, 0, 0, "a page that is not ours goes on"},
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        states[0] = cases[i].first, states[1] = cases[i].after, nstate = commits = 0;
        int got = lazy_fault(0x200000123ull, cases[i].write);
        if (got != cases[i].want || commits != cases[i].commits) {
            printf("FAIL: %s: returned %d, %d commits\n", cases[i].what, got, commits);
            bad++;
        }
    }
    lazy = 0, nstate = 0;
    if (lazy_fault(0x200000123ull, 0) || nstate) printf("FAIL: MKW_LAZY_COMMIT=0 still commits\n"), bad++;
    return bad != 0;
}
"""


def test_lazy_fault_on_each_page_state(tmp_path: Path) -> None:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static int lazy_fault(")
    end = source.index("\n}\n", start) + len("\n}\n")
    (tmp_path / "lazy_fault.inc").write_text(source[start:end], encoding="utf-8")
    harness = tmp_path / "main.c"
    harness.write_text(HARNESS, encoding="utf-8")
    executable = tmp_path / "lazy_fault.exe"
    subprocess.run([cc, "-std=c11", "-Wall", "-Werror", str(harness), "-o", str(executable)], check=True, timeout=30)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
