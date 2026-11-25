"""Check frame generation's requested-step placement on the display's vblank clock, and without it.

Why: one refresh per flip runs 60 -> 120 at 240 Hz twice as fast, while a step that is not a whole number of refreshes
has to alternate one and two refreshes. Late slots also must not reuse a refresh, or mailbox drops one of the pictures.
Without the clock (immediate), a picture must not follow the flip before it within half a step, or a late game frame
is replaced almost at once. The production helper is cut out of vk_backend.c and built with a deterministic vblank
clock; the harness keeps the flip before's call time as flip_main does.
Usage: uv run --with ziglang pytest tests/runtime/test_flip_pace.py
"""

import importlib.util
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

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
    if (argc != 11) return 2;
    FlipPace p = {.first = strtoull(argv[1], NULL, 10), .step = strtoull(argv[2], NULL, 10),
                  .last = strtoull(argv[10], NULL, 10)};
    int n = atoi(argv[3]), late = atoi(argv[4]), held = 0;
    uint64_t delay = strtoull(argv[5], NULL, 10);
    test_refresh = strtoull(argv[6], NULL, 10);
    test_vblank = strtoull(argv[7], NULL, 10) + strtoll(argv[9], NULL, 10);
    p.clock = atoi(argv[8]);
    for (int k = 0; k < n; k++) {
        uint64_t now = k == late ? p.first + (uint64_t)k * p.step + delay : 0;
        uint64_t at = flip_pace_next(&p, k, now, k == 0);
        printf("%llu%c", (unsigned long long)at, k + 1 == n ? '\n' : ' ');
        p.last = at > now ? at : now;   /* flip_main: the flip's call time, after waiting for `at` */
        held += p.held, p.held = 0;
    }
    printf("%d\n", held);
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
    first = "typedef struct { uint64_t first, step, previous"
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


def schedule_held(
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
    last: int | None = None,
) -> tuple[list[int], int]:
    """The call times of a job's n pictures and how many flip_pace_free held. last: the flip before's call time,
    relative to the job's first picture (None: no flip before)."""
    refresh = round(1_000_000_000 / hz) if known else 0
    first = job_first(hz, phase)
    previous = 0 if last is None else first + last
    out = subprocess.run([str(scheduler), str(first), str(step), str(n), str(late), str(delay),
                          str(refresh), str(VBLANK), str(int(clock)), str(jitter), str(previous)],
                         capture_output=True, text=True, check=True)
    calls, held = out.stdout.splitlines()
    return list(map(int, calls.split())), int(held)


def schedule(scheduler: Path, hz: float, n: int, step: int, **kw: Any) -> list[int]:
    return schedule_held(scheduler, hz, n, step, **kw)[0]


def vblank_offsets(calls: list[int], hz: float) -> list[int]:
    refresh = round(1_000_000_000 / hz)
    origin = calls[0]
    return [round((t - origin) / refresh) for t in calls]


def test_240hz_60_to_120_uses_two_refreshes(scheduler: Path) -> None:
    calls = schedule(scheduler, 240, 2, 8_333_333)
    assert vblank_offsets(calls, 240) == [0, 2]
    assert [t for t in calls] == sorted(set(calls))


def test_flip_main_uses_each_job_picture_once() -> None:
    source = SRC.read_text(encoding="utf-8")
    assert "flip_pace_next(&pace, 0, t0, 1)" in source
    assert "if (k > 1) at = flip_pace_next(&pace, k - 1, kern_now_ns(), 0)" in source
    assert "target = flip_pace_next(&pace, f.nf - 1, kern_now_ns(), 0)" in source


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


def test_clock_off_holds_a_picture_half_a_step_after_a_late_flip(scheduler: Path) -> None:
    # 2x at 30 fps on 60 Hz without the clock: the last game frame went out late, 4 ms before this job's made frame was
    # due. The made frame waits until half a step after it; the game frame keeps its time.
    step = 16_666_667
    first = job_first(60)
    calls, held = schedule_held(scheduler, 60, 2, step, clock=False, last=-4_000_000)
    assert [t - first for t in calls] == [-4_000_000 + step // 2, step]
    assert held == 1


def test_clock_off_late_flips_catch_up_half_a_step_each(scheduler: Path) -> None:
    # the flip before went out 10 ms after this job's first picture was due: each picture waits half a step after the
    # one before, none closer, until the requested times are reached again
    step = 8_333_333
    first = job_first(120)
    calls, held = schedule_held(scheduler, 120, 5, step, clock=False, last=10_000_000)
    gaps = [b - a for a, b in zip([first + 10_000_000, *calls], calls)]
    assert all(g >= step // 2 for g in gaps)
    assert calls[-1] == first + 4 * step
    assert held == 4


def test_clock_off_on_time_flips_are_not_held(scheduler: Path) -> None:
    step = 8_333_333
    first = job_first(120)
    calls, held = schedule_held(scheduler, 120, 3, step, clock=False, last=-step)
    assert calls == [first, first + step, first + 2 * step]
    assert held == 0


def test_clock_on_ignores_the_flip_before(scheduler: Path) -> None:
    # on the clock each flip has a refresh of its own: the half-step floor is only for flips without one
    calls = schedule(scheduler, 120, 2, 8_333_333)
    assert schedule_held(scheduler, 120, 2, 8_333_333, last=-1_000_000) == (calls, 0)
