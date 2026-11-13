"""Windows installation selection must leave room below MAX_PATH for prepared files and save/cache paths.

Build the actual _WIN32 cfg.c and discovery code with Clang and MinGW. Run natively in Windows CI, or through Wine on Linux
when both tools are installed, without the game or a GPU. UTF-8 argv is converted through wmain so Unicode boundary
cases exercise the production UTF-16 length check rather than the test process's ANSI code page.
Usage: uv run pytest tests/test_game_dir_windows.py
"""

import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
WINDOWS_LIMIT = 131  # MAX_PATH (260), less the NUL and 128 UTF-16 units for derived runtime filenames


def windows_path(path: Path) -> str:
    return str(path) if os.name == "nt" else "Z:" + str(path.resolve()).replace("/", "\\")


@dataclass
class WindowsProbe:
    exe: Path
    runner: list[str]
    env: dict[str, str]

    def __call__(self, *args: str) -> dict[str, str]:
        # Wine's first process can start services that inherit stdout/stderr. Files avoid waiting for those
        # background services to close a capture pipe after the short-lived probe has already exited.
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            p = subprocess.run([*self.runner, windows_path(self.exe), *args], env=self.env, stdout=out,
                               stderr=err, timeout=60)
            out.seek(0)
            err.seek(0)
            assert p.returncode == 0, err.read().decode("utf-8", errors="replace")
            text = out.read().decode("utf-8")
        return dict(line.split("=", 1) for line in text.splitlines())


@pytest.fixture(scope="module")
def windows_build(tmp_path_factory: pytest.TempPathFactory) -> WindowsProbe:
    env = {k: v for k, v in os.environ.items() if not k.startswith(("MKW_", "TEST_LOG_", "TEST_STDERR_"))}
    if os.name == "nt":
        mingw = shutil.which("gcc")
        for location in ("C:/msys64/mingw64/bin/gcc.exe", "C:/msys64/ucrt64/bin/gcc.exe"):
            if not mingw and Path(location).is_file():
                mingw = location
        runner = []
    else:
        mingw, wine = shutil.which("x86_64-w64-mingw32-gcc"), shutil.which("wine")
        if not wine:
            pytest.skip("Wine is needed to execute the Windows selector on this host")
        runner = [wine]
        env.update(WINEPREFIX=str(ROOT / "build/selector-wine"), WINEDEBUG="-all", DISPLAY="", WAYLAND_DISPLAY="",
                   WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
    cc = shutil.which("clang")
    if not cc or not mingw:
        pytest.skip("Clang and MinGW libraries are needed to compile the runtime's Windows C")
    # The runtime uses Clang vector builtins. GCC supplies only the MinGW headers, CRT and support-library paths.
    libgcc = Path(subprocess.check_output([mingw, "-print-libgcc-file-name"], text=True).strip()).resolve()
    kernel32 = Path(subprocess.check_output([mingw, "-print-file-name=libkernel32.a"], text=True).strip()).resolve()
    exe = tmp_path_factory.mktemp("windows-selector") / "probe.exe"
    includes = [ROOT / "runtime", ROOT / "runtime/cpu", ROOT / "runtime/core/base", ROOT / "runtime/core/boot"]
    sources = [ROOT / "tests/c/t_game_dir.c", ROOT / "runtime/core/base/cfg.c",
               ROOT / "runtime/core/base/sysinfo_log.c", ROOT / "runtime/core/boot/gamelib.c"]
    subprocess.run([cc, "--target=x86_64-w64-windows-gnu", f"--sysroot={kernel32.parent.parent}",
                    f"-L{libgcc.parent}", "-fuse-ld=lld", "-municode",
                    *(f"-I{p}" for p in includes), *(str(p) for p in sources), "-o", str(exe)],
                   check=True, timeout=60)
    return WindowsProbe(exe, runner, env)


@pytest.fixture
def selector(windows_build: WindowsProbe):
    # Keep the launcher itself short even when pytest's generated per-test name is long on Windows runners.
    with tempfile.TemporaryDirectory(prefix="mkw-") as tmp:
        folder = Path(tmp) / "launcher"
        folder.mkdir()
        exe = Path(shutil.copy2(windows_build.exe, folder / "mkw.exe"))
        yield WindowsProbe(exe, windows_build.runner, windows_build.env)


def sized_path(base: Path, units: int, letter: str = "x") -> Path:
    path = base
    width = len(letter.encode("utf-16-le")) // 2
    while units - len(windows_path(path).encode("utf-16-le")) // 2 > 101:
        path /= "x" * 100
    remaining = units - len(windows_path(path).encode("utf-16-le")) // 2 - 1
    assert remaining > 0
    return path / (letter * (remaining // width) + "x" * (remaining % width))


def prepare_folder(path: Path) -> None:
    path.mkdir(parents=True)
    (path / "mkwgame.dll").touch()
    (path / "mkwgame.id").write_text("dev")


@pytest.mark.parametrize("letter", ["x", "ñ", "😀"])
def test_windows_boundary_uses_utf16(selector: WindowsProbe, letter: str) -> None:
    game = sized_path(selector.exe.parent.parent, WINDOWS_LIMIT, letter)
    prepare_folder(game)
    chosen = windows_path(game)
    assert len(chosen.encode("utf-16-le")) // 2 == WINDOWS_LIMIT
    assert selector("select", chosen)["selected"] == "1"
    assert selector()["ready"] == "1"  # prepared-file discovery goes through the Windows filesystem API
    assert selector()["dir"] == chosen
    before = (selector.exe.parent / "mkw.ini").read_bytes()
    result = selector("select", chosen + "x")
    assert result["selected"] == "0" and result["invalid"] == "1"
    assert (selector.exe.parent / "mkw.ini").read_bytes() == before


@pytest.mark.parametrize("units", [260, 300])
def test_windows_long_selection_is_not_saved(selector: WindowsProbe, units: int) -> None:
    game = sized_path(selector.exe.parent.parent, units)
    result = selector("select", windows_path(game))
    assert result["selected"] == "0" and result["invalid"] == "1"
    assert not (selector.exe.parent / "mkw.ini").exists()


def test_windows_relative_path_counts_launcher_prefix(selector: WindowsProbe) -> None:
    target = sized_path(selector.exe.parent, WINDOWS_LIMIT + 1)
    relative = str(target.relative_to(selector.exe.parent))
    assert len(relative) < WINDOWS_LIMIT
    assert selector("select", relative)["invalid"] == "1"
    assert not (selector.exe.parent / "mkw.ini").exists()


def test_windows_invalid_saved_path_allows_recovery(selector: WindowsProbe) -> None:
    ini = selector.exe.parent / "mkw.ini"
    bad = windows_path(sized_path(selector.exe.parent.parent, 300))
    ini.write_text(f'game_dir = "{bad}"\n', encoding="utf-8")
    assert selector()["dir"] == windows_path(selector.exe.parent)
    game = selector.exe.parent.parent / "Recovered"
    prepare_folder(game)
    assert selector("select", windows_path(game))["selected"] == "1"
    assert selector()["ready"] == "1"
