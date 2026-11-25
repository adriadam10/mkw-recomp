"""Check frame generation's requested-step placement on the display's vblank clock.

Why: one refresh per flip runs 60 -> 120 at 240 Hz twice as fast, while a step that is not a whole number of refreshes
has to alternate one and two refreshes. Late slots also must not reuse a refresh, or mailbox drops one of the pictures.
A job's first picture keeps the slot of the first call, which ready_slack waited for: asked for again it went a
refresh late, and in a job of one picture (frame generation's first frame after a pause) that picture is the game's
frame. The production helpers are cut out of vk_backend.c and built with a deterministic vblank clock; the harness
asks for the pictures in flip_main's order.
Usage: uv run --with ziglang pytest tests/runtime/test_flip_pace.py
"""

import importlib.util
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "runtime/gfx/vk/device/vk_backend.c"

STUBS = r"""
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint64_t test_vblank, test_refresh;
static double vk_present_refresh_hz(void) { return test_refresh ? 1e9 / (double)test_refresh : 0; }
static uint64_t vk_present_flip_time(uint64_t t) {
    if (!test_refresh) return 0;
    double k = ceil(((double)t + (double)test_refresh * 0.5 - (double)test_vblank) /
                    (double)test_refresh);
    return (uint64_t)((double)test_vblank + k * (double)test_refresh - (double)test_refresh * 0.5);
}
"""

MAIN = r"""
int main(int argc, char **argv) {
    if (argc != 10) return 2;
    FlipPace p = {strtoull(argv[1], NULL, 10), strtoull(argv[2], NULL, 10), 0, 1};
    int n = atoi(argv[3]), late = atoi(argv[4]);
    uint64_t delay = strtoull(argv[5], NULL, 10);
    test_refresh = strtoull(argv[6], NULL, 10);
    test_vblank = strtoull(argv[7], NULL, 10) + strtoll(argv[9], NULL, 10);
    p.clock = atoi(argv[8]);
    /* flip_main's order: the first call places picture 0 before ready_slack's wait, then each picture is flipped */
    uint64_t at0 = flip_pace_next(&p, 0, late == 0 ? p.first + delay : 0, 1);
    for (int k = 0; k < n; k++) {
        uint64_t now = k == late ? p.first + (uint64_t)k * p.step + delay : 0;
        printf("%llu%c", (unsigned long long)flip_pace_picture(&p, k, at0, now),
               k + 1 == n ? '\n' : ' ');
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def scheduler(tmp_path_factory: pytest.TempPathFactory) -> Path:
    cc = shutil.which("clang") or shutil.which("cc") or shutil.which("gcc")
    command = [cc] if cc else []
    if not command:
        zig = shutil.which("zig")
        spec = importlib.util.find_spec("ziglang")
        if not zig and spec and spec.origin:
            zig = str(Path(spec.origin).parent / ("zig.exe" if os.name == "nt" else "zig"))
        if not zig:
            pytest.skip("no C compiler (use uv run --with ziglang pytest tests/runtime/test_flip_pace.py)")
        command = [zig, "cc"]
    source = SRC.read_text(encoding="utf-8")
    first = "typedef struct { uint64_t first, step, previous;"
    last = "static void trace_present("
    assert first in source and last in source, "flip pacing helper moved: update this harness"
    path = tmp_path_factory.mktemp("flip-pace") / "flip_pace.c"
    path.write_text(STUBS + source[source.index(first) : source.index(last)] + MAIN, encoding="utf-8")
    exe = path.with_suffix(".exe")
    libm = [] if os.name == "nt" else ["-lm"]
    subprocess.run([*command, "-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(exe), *libm],
                   check=True)
    return exe


VBLANK = 10_000_000_000


def job_first(hz: float, phase: int = 0) -> int:
    """The first picture's requested time in schedule(): half a refresh after a vblank, plus phase."""
    return VBLANK + round((1_000_000_000 / hz) / 2) + phase


def schedule(
    scheduler: Path,
    hz: float,
    n: int,
    step: int,
    late: int = -1,
    delay: int = 0,
    clock: bool = True,
    phase: int = 0,
    jitter: int = 0,
    known: bool = True,
) -> list[int]:
    refresh = round(1_000_000_000 / hz) if known else 0
    first = job_first(hz, phase)
    out = subprocess.run([str(scheduler), str(first), str(step), str(n), str(late), str(delay),
                          str(refresh), str(VBLANK), str(int(clock)), str(jitter)],
                         capture_output=True, text=True, check=True)
    return list(map(int, out.stdout.split()))


def vblank_offsets(calls: list[int], hz: float) -> list[int]:
    refresh = round(1_000_000_000 / hz)
    origin = calls[0]
    return [round((t - origin) / refresh) for t in calls]


def test_240hz_60_to_120_uses_two_refreshes(scheduler: Path) -> None:
    calls = schedule(scheduler, 240, 2, 8_333_333)
    assert vblank_offsets(calls, 240) == [0, 2]
    assert [t for t in calls] == sorted(set(calls))


def test_flip_main_uses_each_job_picture_once() -> None:
    # the harness's order: the first call, then flip_pace_picture for each made frame and for the game's
    source = SRC.read_text(encoding="utf-8")
    assert "at0 = flip_pace_next(&pace, 0, t0, 1)" in source
    assert "at = flip_pace_picture(&pace, k - 1, at0, kern_now_ns())" in source
    assert "target = flip_pace_picture(&pace, f.nf - 1, at0, kern_now_ns())" in source
    assert source.count("flip_pace_next(&pace") == 1


def test_one_picture_job_flips_the_game_frame_at_its_slot(scheduler: Path) -> None:
    # frame generation's first frame after a pause, a menu or a gap makes no frame: the job is the game's frame alone,
    # picture 0, which the first call placed and ready_slack waited for. Asked for again, the slot was taken and the
    # game frame went a refresh late (at 30 fps with 2x on 60 Hz it was shown one refresh instead of two).
    for hz in (60, 119.88, 144):
        (call,) = schedule(scheduler, hz, 1, 16_666_667)
        assert 0 <= call - job_first(hz) < round(1_000_000_000 / hz)   # the first slot at or after its time


def test_late_one_picture_job_keeps_the_slot_after_now(scheduler: Path) -> None:
    # its time had passed when the flip thread took it: the first slot after now, not the one after that
    refresh = 16_666_667
    assert schedule(scheduler, 60, 1, refresh, late=0, delay=5_000_000) == [job_first(60) + refresh]


def test_119_88hz_two_flips_use_two_vblanks(scheduler: Path) -> None:
    calls = schedule(scheduler, 119.88, 2, 16_666_667)
    assert vblank_offsets(calls, 119.88) == [0, 2]


def test_imperfect_divisor_balances_one_and_two_refresh_gaps(scheduler: Path) -> None:
    calls = schedule(scheduler, 144, 6, 8_333_333)
    offsets = vblank_offsets(calls, 144)
    gaps = [b - a for a, b in zip(offsets, offsets[1:])]
    assert offsets == [0, 1, 2, 4, 5, 6]
    assert set(gaps) == {1, 2}
    assert sum(gaps) == 6


def test_late_recovery_never_reuses_a_flip_slot(scheduler: Path) -> None:
    calls = schedule(scheduler, 120, 5, 8_333_333, late=2, delay=5_000_000)
    offsets = vblank_offsets(calls, 120)
    assert offsets == [0, 1, 3, 4, 5]
    assert calls == sorted(set(calls))


def test_small_predicted_phase_jitter_keeps_two_vblanks(scheduler: Path) -> None:
    for jitter in (-400_000, -1, 1, 400_000):
        calls = schedule(scheduler, 119.88, 4, 16_666_667, jitter=jitter)
        assert vblank_offsets(calls, 119.88) == [0, 2, 4, 6]


def test_clock_off_keeps_requested_steps(scheduler: Path) -> None:
    calls = schedule(scheduler, 240, 2, 8_333_333, late=1, delay=1_000_000, clock=False)
    assert calls[1] - calls[0] == 8_333_333


def test_clock_unknown_keeps_requested_steps(scheduler: Path) -> None:
    calls = schedule(scheduler, 120, 4, 4_000_000, known=False)
    assert [b - a for a, b in zip(calls, calls[1:])] == [4_000_000] * 3


def test_step_shorter_than_refresh_does_not_reuse_a_call(scheduler: Path) -> None:
    calls = schedule(scheduler, 120, 5, 4_000_000)
    refresh = round(1_000_000_000 / 120)
    assert calls == sorted(set(calls))
    assert all(b - a >= refresh for a, b in zip(calls, calls[1:]))
