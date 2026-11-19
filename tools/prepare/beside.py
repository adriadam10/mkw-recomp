"""A step of prepare.py run in a thread beside the ones before it in the list: its tools take the cores those steps
leave idle, and the start screen still sees one step at a time.

Why: an update on the Windows PC (6 threads) spent 390 s in steps one after another, with the cores idle in the
seeding of setup-next/spirv (46 297 hard links, 17 s), in compile (18 s at 8-40 % CPU) and in extract.py (17 s, one
process) (18/11/2026). The shaders step depends on none of generate and compile.

The thread's progress lines are held until the main loop reaches the step (show): the start screen draws one bar,
the step it was last told. Not run on its own.
"""

import sys
import threading
import time
from collections.abc import Callable

from machine import emit


class Beside:
    def __init__(self, name: str, job: Callable[["Beside"], None]) -> None:
        self.name, self.error, self._held, self._shown = name, None, None, False
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, args=(job,), name=name, daemon=True)
        self._thread.start()

    def _run(self, job: Callable[["Beside"], None]) -> None:
        t0 = time.monotonic()
        try:
            job(self)
        except BaseException as e:  # noqa: BLE001 - raised again in the main thread by join
            self.error = e
        print(f"time: {self.name}-beside {time.monotonic() - t0:.1f} s", file=sys.stderr)

    def progress(self, line: str) -> None:
        with self._lock:
            if self._shown:
                emit(line)
            else:
                self._held = line

    def join(self, show: bool = False) -> None:
        """Waits for the job (its error raised here); show: its progress goes to the start screen from now on."""
        if show:
            with self._lock:
                self._shown = True
                if self._held:
                    emit(self._held)
        self._thread.join()
        if self.error is not None:
            raise self.error
