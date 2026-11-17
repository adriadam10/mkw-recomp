"""Compile the production frame-generation rate selector with display/game stubs, without Vulkan or game files.

Why: a 120 fps output target must adapt between 60x2 and 30x4, respect display/slot limits, and preserve the
existing automatic modes, movie bypass and uneven-refresh warning.
Usage: uv run pytest tests/runtime/test_framegen.py (or uv run --with ziglang pytest tests/test_framegen.py without a C compiler).
"""

import importlib.util
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "runtime/gfx/vk/device/vk_framegen.c"

STUBS = r"""
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_FG_MAX 4
static const char *setting;
static int iv, room, variable, movie, turbo;
static double hz;
static struct { uint64_t interval, frames; int auto_m; } F;
static int frame_present_interval(void) { return iv; }
static double vk_present_refresh_hz(void) { return hz; }
static int vk_present_ahead_max(void) { return room; }
static int cfg_bool(const char *key) { (void)key; return variable; }
static const char *cfg_value(const char *key) { (void)key; return setting; }
static int turbo_speed(void) { return turbo; }
static int ov_movie_playing(void) { return movie; }
"""

MAIN = r"""
int main(int argc, char **argv) {
    if (argc != 8) return 2;
    setting = argv[1]; iv = atoi(argv[2]); hz = atof(argv[3]); room = atoi(argv[4]);
    variable = atoi(argv[5]); movie = atoi(argv[6]); turbo = atoi(argv[7]);
    F.frames = 1; F.interval = (uint64_t)(game_ticks() * (1e9 / 60.0));
    printf("%d %d\n", mult_now(), vk_fg_uneven());
    return 0;
}
"""


@pytest.fixture(scope="module")
def selector(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    command = [cc] if cc else []
    if not command:
        zig = shutil.which("zig")
        spec = importlib.util.find_spec("ziglang")
        if not zig and spec and spec.origin:
            zig = str(Path(spec.origin).parent / ("zig.exe" if os.name == "nt" else "zig"))
        if not zig:
            pytest.skip("no C compiler (use uv run --with ziglang pytest tests/test_framegen.py)")
        command = [zig, "cc"]
    src = SRC.read_text(encoding="utf-8")
    first, last = "static int game_ticks(void)", "// MKW_FG_DUMP="
    assert first in src and last in src, "frame-generation selector moved: update this harness"
    d = tmp_path_factory.mktemp("framegen")
    c = d / "selector.c"
    c.write_text(STUBS + src[src.index(first) : src.index(last)] + MAIN, encoding="utf-8")
    exe = d / "selector.exe"
    libm = [] if os.name == "nt" else ["-lm"]
    subprocess.run([*command, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe), *libm], check=True)
    return exe


@pytest.mark.parametrize(
    ("mode", "iv", "hz", "room", "vrr", "movie", "turbo", "expected"),
    [
        ("120", 1, 120, 4, 0, 0, 1, (2, 0)),
        ("120", 2, 120, 4, 0, 0, 1, (4, 0)),
        ("120", 1, 240, 4, 0, 0, 1, (2, 0)),  # target stays 120 on a faster display
        ("120", 2, 240, 4, 0, 0, 1, (4, 0)),
        ("120", 1, 119.88, 4, 0, 0, 1, (2, 0)),
        ("120", 2, 119.88, 4, 0, 0, 1, (4, 0)),
        ("120", 1, 60, 4, 0, 0, 1, (1, 0)),  # do not make frames a 60 Hz panel cannot show
        ("120", 2, 59.94, 4, 0, 0, 1, (2, 0)),
        ("120", 1, 144, 4, 0, 0, 1, (2, 1)),
        ("120", 1, 144, 4, 1, 0, 1, (2, 0)),
        ("120", 2, 120, 2, 0, 0, 1, (2, 0)),  # swapchain holds only two frames
        ("120", 2, 120, 1, 0, 0, 1, (1, 0)),
        ("120", 3, 120, 4, 0, 0, 1, (4, 1)),  # 4x limit: 20 -> 80, uneven at 120 Hz
        ("120", 4, 120, 4, 0, 0, 1, (4, 0)),  # 15 -> 60
        ("120", 1, 0, 4, 0, 0, 1, (2, 0)),  # unknown display rate
        ("120", 0, 120, 4, 0, 0, 1, (2, 0)),
        ("120", 1, 120, 4, 0, 1, 1, (0, 0)),
        ("120", 1, 120, 4, 0, 0, 2, (0, 0)),
        ("off", 1, 120, 4, 0, 0, 1, (0, 0)),
        ("auto", 1, 240, 4, 0, 0, 1, (4, 0)),
        ("auto", 2, 60, 4, 0, 0, 1, (2, 0)),
        ("auto", 1, 75, 4, 0, 0, 1, (1, 0)),
        ("auto", 1, 144, 4, 1, 0, 1, (2, 0)),
        ("2", 1, 120, 4, 0, 0, 1, (2, 0)),
        ("3", 2, 60, 4, 0, 0, 1, (3, 1)),
        ("4", 2, 120, 2, 0, 0, 1, (2, 0)),
    ],
)
def test_output_rate(
    selector: Path,
    mode: str,
    iv: int,
    hz: float,
    room: int,
    vrr: int,
    movie: int,
    turbo: int,
    expected: tuple[int, int],
) -> None:
    args = [str(selector), mode, str(iv), str(hz), str(room), str(vrr), str(movie), str(turbo)]
    out = subprocess.run(args, capture_output=True, text=True, check=True)
    assert tuple(map(int, out.stdout.split())) == expected
