"""The game code's chunks on LLVM's clang with zig's -cc1 line (tools/prepare/fastcc.py), with a stand-in zig and clang."""

import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/prepare"))
import fastcc

pytestmark = pytest.mark.skipif(os.name == "nt", reason="shell scripts stand in for zig and clang")


def stand_ins(tmp: Path, clang_version: str, windows: bool = False) -> tuple[list[str], Path]:
    """A zig whose -### prints a -cc1 line naming its input and output, and a package with a clang. windows: as
    zig.exe, the output is a temporary in its cache (also the CodeView name) and it exits 1 on renaming it."""
    zig = tmp / "zig"
    tmp_out = '"-object-file-name=/c/tmp/ab12-\'"$(basename "$out" .o)"\'.obj" ' if windows else ""
    out = '/c/tmp/ab12-\'"$(basename "$out" .o)"\'.obj' if windows else "'\"$out\"'"
    zig.write_text(
        "#!/bin/sh\n"
        'case "$*" in *--version*) echo "clang version 22.1.8"; exit 0;; esac\n'
        'for a; do [ "$prev" = -o ] && out=$a; case $a in *.c) src=$a;; esac; prev=$a; done\n'
        'echo \'"/z/zig" "-cc1" "-main-file-name" "\'"$(basename "$src")"\'" "-D" "ID=\\"x\\"" '
        f'{tmp_out}"-o" "{out}" "-x" "c" "\'"$src"\'"\' >&2\n'
        + ("echo \"error: failed to rename 'tmp' into 'o': FileNotFound\" >&2; exit 1\n" if windows else "")
    )
    pkg = tmp / "setup"
    (pkg / "clang").mkdir(parents=True)
    clang = fastcc.clang_path(pkg)
    clang.write_text(f'#!/bin/sh\necho "clang version {clang_version} (https://github.com/llvm/llvm-project abc)"\n')
    for p in (zig, clang):
        p.chmod(0o755)
    return [str(zig), "cc", "-target", "x86_64-linux-gnu"], pkg


def test_a_chunk_runs_zigs_line_on_clang_with_its_paths(tmp_path: Path) -> None:
    cc, pkg = stand_ins(tmp_path, "22.1.8")
    obj = tmp_path / "obj"
    obj.mkdir()
    cc1 = fastcc.setup(cc, pkg, obj, [["-O1"]])
    assert cc1 is not None
    src, out = tmp_path / "gen/main_0001.c", obj / "main_main_0001.o"
    cmd = cc1.cmd(["-O1"], src, out)
    assert cmd[0] == str(fastcc.clang_path(pkg))
    assert cmd[1:] == ["-cc1", "-main-file-name", "main_0001.c", "-D", 'ID="x"', "-o", str(out), "-x", "c", str(src)]
    assert not (obj / f"{fastcc.PROBE}.c").exists()


def test_zig_exe_line_writes_the_chunk_object_not_its_cache_temporary(tmp_path: Path) -> None:
    cc, pkg = stand_ins(tmp_path, "22.1.8", windows=True)
    obj = tmp_path / "obj"
    obj.mkdir()
    cc1 = fastcc.setup(cc, pkg, obj, [["-O1"]])
    assert cc1 is not None
    src, out = tmp_path / "gen/main_0001.c", obj / "main_main_0001.o"
    assert cc1.cmd(["-O1"], src, out)[1:] == [
        "-cc1", "-main-file-name", "main_0001.c", "-D", 'ID="x"', f"-object-file-name={out}", "-o", str(out),
        "-x", "c", str(src),
    ]  # fmt: skip


def test_zig_compiles_when_clang_is_another_version(tmp_path: Path) -> None:
    cc, pkg = stand_ins(tmp_path, "22.1.7")
    assert fastcc.setup(cc, pkg, tmp_path, [["-O1"]]) is None


def test_zig_compiles_when_asked(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    cc, pkg = stand_ins(tmp_path, "22.1.8")
    monkeypatch.setenv("MKW_SETUP_CC", "zig")
    assert fastcc.setup(cc, pkg, tmp_path, [["-O1"]]) is None


def wine_stand_ins(tmp: Path, extra_error: str = "") -> tuple[list[str], Path]:
    """zig.exe as under Wine (Proton 11.0, 20/11/2026): it cannot lock its cache's manifest, so a compile ends in
    "checking cache failed" and -### prints its line first and then the same error, both with exit 1. The clang beside
    it writes its arguments to clang.args. extra_error: one more error line, as a wrong flag would give."""
    cc, pkg = stand_ins(tmp, "22.1.8")
    zig = Path(cc[0])
    lines = zig.read_text().splitlines()
    fail = (
        "fail() {\n"
        '  echo "$src:1:1: error: checking cache failed: manifest_create Unexpected" >&2\n'
        + (f'  echo "{extra_error}" >&2\n' if extra_error else "")
        + "  exit 1\n}\n"
    )
    # lines[:3]: the shebang, --version and the loop that reads the paths; then a compile (no -###) fails at once,
    # and -### prints its line (lines[3]) before failing
    zig.write_text(
        "\n".join(lines[:3]) + "\n" + fail + 'case "$*" in *"-###"*) ;; *) fail;; esac\n' + lines[3] + "\nfail\n"
    )
    clang = fastcc.clang_path(pkg)
    clang.write_text(
        "#!/bin/sh\n"
        'case "$*" in *--version*) echo "clang version 22.1.8 (https://github.com/llvm/llvm-project abc)"; exit 0;; esac\n'
        f'printf "%s\\n" "$@" > "{tmp}/clang.args"\n'
    )
    return cc, pkg


def test_zigs_line_is_taken_when_it_only_failed_on_its_cache(tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    cc, pkg = wine_stand_ins(tmp_path)
    obj = tmp_path / "obj"
    obj.mkdir()
    cc1 = fastcc.setup(cc, pkg, obj, [["-O1"]])
    assert cc1 is not None and cc1.no_cache
    src, out = tmp_path / "gen/main_0001.c", obj / "main_main_0001.o"
    assert cc1.cmd(["-O1"], src, out)[1:] == [
        "-cc1", "-main-file-name", "main_0001.c", "-D", 'ID="x"', "-o", str(out), "-x", "c", str(src),
    ]  # fmt: skip
    assert "checking cache failed" in capsys.readouterr().err  # setup.log says why zig did not compile


def test_zigs_line_is_refused_with_another_error_beside_the_cache(tmp_path: Path) -> None:
    cc, pkg = wine_stand_ins(tmp_path, "error: unknown argument: '-fnope'")
    assert fastcc.setup(cc, pkg, tmp_path, [["-O1"]]) is None


def test_cache_failed_wants_every_error_to_be_the_cache() -> None:
    cache = r"Z:\g\p.c:1:1: error: checking cache failed: manifest_create Unexpected"
    assert fastcc.cache_failed(cache)
    assert fastcc.cache_failed(f'clang version 22.1.8\n "zig.exe" "-cc1" "-D" "X=\\"error:\\""\n{cache}\n')
    assert not fastcc.cache_failed("")
    assert not fastcc.cache_failed("p.c:1:1: error: expected ';'")
    assert not fastcc.cache_failed(f"{cache}\np.c:1:1: error: expected ';'")


def test_compile_c_runs_clang_on_zigs_line_when_zig_fails_on_its_cache(tmp_path: Path) -> None:
    cc, pkg = wine_stand_ins(tmp_path)
    work = tmp_path / "work"
    work.mkdir()
    src, out = tmp_path / "intercept_win.c", work / "intercept_win.o"
    fastcc.compile_c(cc, pkg, work, ["-O2"], src, out)
    args = (tmp_path / "clang.args").read_text().splitlines()
    assert args == ["-cc1", "-main-file-name", "intercept_win.c", "-D", 'ID="x"', "-o", str(out), "-x", "c", str(src)]


def test_compile_c_leaves_a_working_zig_alone(tmp_path: Path) -> None:
    cc, pkg = wine_stand_ins(tmp_path)
    zig = Path(cc[0])
    zig.write_text("#!/bin/sh\nexit 0\n")  # compiles: nothing asked of clang
    fastcc.compile_c(cc, pkg, tmp_path, ["-O2"], tmp_path / "a.c", tmp_path / "a.o")
    assert not (tmp_path / "clang.args").exists()


def test_compile_c_fails_with_zigs_own_error(tmp_path: Path) -> None:
    cc, pkg = wine_stand_ins(tmp_path)
    Path(cc[0]).write_text("#!/bin/sh\necho \"a.c:1:1: error: expected ';'\" >&2\nexit 1\n")
    with pytest.raises(subprocess.CalledProcessError):
        fastcc.compile_c(cc, pkg, tmp_path, ["-O2"], tmp_path / "a.c", tmp_path / "a.o")
    assert not (tmp_path / "clang.args").exists()
