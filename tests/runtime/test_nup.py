"""The runtime's .nup reader (runtime/sdk/fs/nup.c) against the setup's (tools/prepare/nup.py) on packages of several
titles, as Eglantine's --pack-nup writes the base game and an update: both layer the titles the same way (an update's
RomFS over the base game's, its ExeFS whole, DLC left out, by version). nup.c is built alone with t_nup.c, which stubs
the platform layer on stdio; zstd is stubbed too, so the packages here store files raw (tests/launcher/test_setup.py
reads a zstd frame back through nup.py).

Usage: uv run pytest tests/runtime/test_nup.py   (CC names the compiler, else clang, cc or gcc on PATH)
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/prepare"))
sys.path.insert(0, str(ROOT / "tests/launcher"))
import nup  # noqa: E402
from test_setup import layered_nup  # noqa: E402

MKW = 0x0400C3F00006E000
STUB_HOST = "#pragma once\nvoid rt_log(const char *fmt, ...);\n"
# nup.c's decompression path, never reached here (no zstd frames): enough to compile it
STUB_ZSTD = """#pragma once
#include <stddef.h>
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
typedef struct { const void *src; size_t size, pos; } ZSTD_inBuffer;
typedef struct { void *dst; size_t size, pos; } ZSTD_outBuffer;
enum { ZSTD_d_windowLogMax = 100 };
static inline ZSTD_DCtx *ZSTD_createDCtx(void) { return NULL; }
static inline size_t ZSTD_freeDCtx(ZSTD_DCtx *z) { (void)z; return 0; }
static inline size_t ZSTD_DCtx_setParameter(ZSTD_DCtx *z, int p, int v) { (void)z; (void)p; (void)v; return 0; }
static inline unsigned ZSTD_isError(size_t r) { (void)r; return 1; }
static inline size_t ZSTD_decompressStream(ZSTD_DCtx *z, ZSTD_outBuffer *o, ZSTD_inBuffer *i) {
    (void)z; (void)o; (void)i; return (size_t)-1;
}
"""


@pytest.fixture(scope="module")
def t_nup(tmp_path_factory: pytest.TempPathFactory) -> Path:
    # CC may be a command with arguments ("zig cc"); a compiler found on PATH is one path, spaces and all
    # ("C:\Program Files\LLVM\bin\clang.EXE" on the Windows runner)
    found = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    cc = os.environ["CC"].split() if os.environ.get("CC") else [found] if found else None
    if not cc:
        pytest.skip("no C compiler")
    out = tmp_path_factory.mktemp("t_nup")
    (out / "host.h").write_text(STUB_HOST)
    (out / "zstd.h").write_text(STUB_ZSTD)
    exe = out / ("t_nup.exe" if os.name == "nt" else "t_nup")
    # clang on Windows targets MSVC by default, whose CRT marks fopen and strdup deprecated; the runtime builds with
    # llvm-mingw, where they are not, so -Werror would fail on the CRT's notice, not on the code
    crt = ["-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_DEPRECATE"] if os.name == "nt" else []
    subprocess.run([*cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", *crt, f"-I{out}",
                    f"-I{ROOT / 'runtime'}", f"-I{ROOT / 'runtime/sdk/fs'}", str(ROOT / "tests/runtime/t_nup.c"),
                    str(ROOT / "runtime/sdk/fs/nup.c"), "-o", str(exe)], check=True)  # fmt: skip
    return exe


def listing(exe: Path, pkg: Path) -> list[str]:
    p = subprocess.run([str(exe), str(pkg)], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    return p.stdout.splitlines()


def expected(pkg: Path) -> list[str]:
    n = nup.Nup(pkg)
    return [f"{part}\t{k}\t{n.read(e).decode()}" for part in ("exefs", "romfs") for k, e in n.files(part).items()]


BASE = {"exefs/main": b"NSO0 1.0.0", "exefs/subsdk0": b"old", "romfs/a.bin": b"a100", "romfs/Sub/b": b"b100",
        "romfs/gone": b"only in 1.0.0", "romfs/Sub/Deep/c": b"c100"}  # fmt: skip
UPDATE = {"exefs/main": b"NSO0 1.8.0", "exefs/subsdk0": b"movie", "exefs/subsdk1": b"old", "romfs/a.bin": b"a180",
          "romfs/Sub/new": b"new", "romfs/Sub/Deep/c": b"c180"}  # fmt: skip


@pytest.mark.parametrize(
    "titles",
    [
        [(MKW, 0, BASE), (MKW, 917504, UPDATE)],  # Eglantine's order: base, then update, one title id
        [(MKW | 0x800, 917504, UPDATE), (MKW | 0x1001, 65536, {"romfs/a.bin": b"dlc"}), (MKW, 0, BASE)],
        [(MKW, 0, BASE), (MKW, 917504, {"romfs/a.bin": b"a180"})],  # an update without its own ExeFS
        [(MKW, 0, BASE)],  # one title, as a dump or nup.py pack: read as before
    ],
    ids=["eglantine", "dlc-and-order", "romfs-only-update", "one-title"],
)
def test_runtime_layers_titles_as_the_setup(t_nup: Path, tmp_path: Path, titles: list) -> None:
    pkg = tmp_path / "g.nup"
    layered_nup(pkg, titles)
    got = listing(t_nup, pkg)
    assert got == expected(pkg)
    if len(titles) > 1:
        assert "romfs\ta.bin\ta180" in got and "romfs\tSub/b\tb100" in got and "romfs\tgone\tonly in 1.0.0" in got
