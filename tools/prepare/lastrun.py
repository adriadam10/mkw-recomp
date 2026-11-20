"""How the last game run ended, for the start screen: read from its log (logs/mkw.log, which the next Play moves to
mkw.1.log) and matched against the known errors of tools/github/signatures.toml, the list the issue bot answers with.

Why: by 14/11/2026 issues came in at 40-68 a day, 15 % of them a known error already tracked or fixed in a newer
release. Told at the next start that it is known (and where it is followed, or that an update fixes it), a player
need not open another issue; an unknown error points to the bug form with the log.

Output: one line on stdout, run with the package's Python by runtime/ui/launcher/launcher_lastrun.cpp:
  "clean"                    the run ended normally (window closed, or the game exited)
  "known <issue>"            a known error; the issue that follows it
  "fixed <issue> <tag>"      a known error fixed in a release newer than --version
  "crash"                    it stopped with an error no signature names
  nothing                    no log, or it ended without a word (killed, power off): nothing to say
Exit code 0 unless the arguments are wrong.

Usage: python -I lastrun.py --game DIR --version VER
"""

import argparse
import re
import sys
import tomllib
from pathlib import Path

SIGNATURES = Path(__file__).resolve().parents[1] / "github/signatures.toml"
TAIL = 16 << 20  # bytes read from the end of the log: a log stops at 64 MiB, the error is at its end
CLEAN = ("gfx: window closed; exiting", "guest ExitProcess at")  # runtime/gfx/nvn/host/gfx_present.c, core/kernel/svc.c
CRASH = re.compile(r"FATAL|device lost|DEVICE_LOST|vk: device fault|fatal signal", re.I)
HANG = "vk: the GPU has not finished submission"  # runtime/gfx/vk/device/vk_sync.c: the watchdog, after 5 s without progress
SLOW = "it was slow, not hung"  # the same file, logged once such a wait ends: the hang was a long submission


def vkey(tag: str) -> tuple[int, ...]:
    return tuple(int(p) for p in re.findall(r"\d+", tag)[:3])


def read_tail(path: Path) -> str:
    with path.open("rb") as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - TAIL))
        return f.read().decode("utf-8", "replace")


def verdict(log: str, version: str, sigs: list[dict]) -> str:
    # A hang the player then closed the window on is not a clean end (#382, #206): the last hang counts when no
    # "slow, not hung" line follows it.
    hung = log.rfind(HANG) > log.rfind(SLOW)
    closed = max(log.rfind(c) for c in CLEAN)
    # Shutdown still runs plugin and save cleanup after the close line; that work can fail.
    if not hung and closed >= 0 and not CRASH.search(log, closed):
        return "clean"
    low = log.lower()
    for s in sigs:
        if all(w.lower() in low for w in s["all"]):
            fixed = s.get("fixed")
            if fixed and vkey(fixed) > vkey(version):
                return f"fixed {s['issue']} {fixed}"
            return f"known {s['issue']}"
    return "crash" if hung or CRASH.search(log) else ""


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", required=True, type=Path)
    ap.add_argument("--version", required=True)
    a = ap.parse_args()
    log = a.game / "logs/mkw.log"
    if not log.is_file():
        return
    sigs = tomllib.loads(SIGNATURES.read_text())["signature"] if SIGNATURES.is_file() else []
    out = verdict(read_tail(log), a.version, sigs)
    if out:
        print(out, flush=True)


if __name__ == "__main__":
    sys.exit(main())
