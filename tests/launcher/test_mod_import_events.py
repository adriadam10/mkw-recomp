"""Imported names must reach the Mods page intact, including their spaces.

Why: the importer permits spaces but its success reader split on the first one. Compile the actual on_line function
and feed it real importer output and error controls. No game or SDL. Usage: uv run pytest tests/launcher/test_mod_import_events.py
"""

import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def events_probe(tmp_path_factory):
    cc = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
    if not cc:
        pytest.skip("a C++ compiler is needed for the production event parser")
    folder = tmp_path_factory.mktemp("mod-events")
    mods = (ROOT / "runtime/ui/launcher/mods.cpp").read_text(encoding="utf-8")
    start = mods.index("static void on_line(char *line) {")
    actual = mods[start:mods.index("\nstatic int SDLCALL reader", start)]
    # Compile production code, following test_launcher_paths.py. Only the SDL mutexes and the state storage are
    # stubbed: these tests do not draw the UI or start a process through SDL.
    source = r'''
#include <cstdio>
#include <cstring>
static struct { void *lock; char done[64], code[32], error[512]; } G;
static void SDL_LockMutex(void *) {}
static void SDL_UnlockMutex(void *) {}
'''
    source += actual + r'''
int main() {
    char line[2048];
    if (!fgets(line, sizeof line, stdin)) return 2;
    line[strcspn(line, "\r\n")] = 0;
    on_line(line);
    printf("%s\t%s\t%s\n", G.done, G.code, G.error);
}
'''
    code = folder / "probe.cpp"
    code.write_text(source, encoding="utf-8")
    exe = folder / "probe.exe"
    subprocess.run([cc, "-std=c++17", str(code), "-o", str(exe)], check=True, timeout=60)
    return exe


def parse(probe, line):
    result = subprocess.run([str(probe)], input=line, capture_output=True, check=True, timeout=10)
    return result.stdout.decode("utf-8").rstrip("\r\n").split("\t", 2)


@pytest.mark.parametrize("name", ["Plain", "Synthetic Mod", "Two  Spaces", "romfs word", "Café Mod", "玩家 模组"])
def test_actual_import_result_retains_the_installed_name(events_probe, tmp_path, name):
    game, vanilla, source = tmp_path / "installation", tmp_path / "vanilla", tmp_path / "source"
    vanilla.mkdir()
    (source / "romfs").mkdir(parents=True)
    (source / "romfs/invented.txt").write_bytes(b"invented mod payload")
    # Force UTF-8 for this parser test on Windows too; output encoding is a separate concern.
    result = subprocess.run([sys.executable, "-I", "-X", "utf8", str(ROOT / "tools/mod/import.py"),
                             "--game", str(game), "--romfs", str(vanilla), "--folder", str(source),
                             "--build-id", "0123456789abcdef", "--name", name],
                            capture_output=True, check=True, timeout=30)
    installed = json.loads((game / "mods" / name / "mod.json").read_text(encoding="utf-8"))
    assert parse(events_probe, result.stdout) == [installed["name"], "", ""]


@pytest.mark.parametrize(("line", "expected"), [
    (b"done Plain\n", ["Plain", "", ""]),
    (b"done Two Words\n", ["Two Words", "", ""]),
    (b"error layout no romfs/ or exefs/ inside\n", ["", "layout", "no romfs/ or exefs/ inside"]),
    (b"error variants One\tTwo Words\n", ["", "variants", "One\tTwo Words"]),
    (b"error layout\n", ["", "layout", ""]),
    (b"done\n", ["", "", ""]),
    (b"unknown ignored\n", ["", "", ""]),
])
def test_protocol_controls(events_probe, line, expected):
    assert parse(events_probe, line) == expected
