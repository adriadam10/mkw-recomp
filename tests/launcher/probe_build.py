"""Compile a test probe with a hang guard: the Windows MinGW probes (test_game_dir_windows.py, test_launcher_paths.py).

Why: on the windows-2025-vs2026 runner image 20261116.315 the wait for a probe's clang sometimes never returns, though
no clang or lld process is left (a dump of the live processes at the timeout, 18/11/2026: 2 of 4 runners of that image,
one round in three each; 0 in about 170 rounds of the 20261103.298 image). The other builds of the same run and the
next round finish in about a second, so a hung attempt is killed with its process tree and the build is tried again,
and each hang is written to stderr (shown in the test report) to keep counting them.
"""

import os
import subprocess
import sys

ATTEMPT_SECONDS = 30  # one build takes 0.5-1 s on the runners (345 timed in CI, 18/11/2026)
ATTEMPTS = 3


def build(cmd: list[str]) -> None:
    for attempt in range(1, ATTEMPTS + 1):
        p = subprocess.Popen(cmd)
        try:
            rc = p.wait(timeout=ATTEMPT_SECONDS)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
            p.kill()
            sys.stderr.write(f"probe_build: attempt {attempt} hung after {ATTEMPT_SECONDS} s: {cmd[0]}\n")
            continue
        if rc:
            raise subprocess.CalledProcessError(rc, cmd)
        return
    raise subprocess.TimeoutExpired(cmd, ATTEMPT_SECONDS * ATTEMPTS)
