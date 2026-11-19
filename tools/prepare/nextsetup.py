"""An update set up beside the version still played: prepare.py --next writes the outputs the game reads into
<game>/setup-next/, and the next prepare.py run of the new version moves them into place before its steps.

Why: an update ran prepare.py in the game folder itself, so mkwgame and spirv/ were half rewritten until it ended and
the player could not play meanwhile (#429: "no way to just go back to the menu and play the version you already have").
The steps write only these outputs where the runtime reads them; the rest (exefs, the RomFS, setup-work with its
objects, the SM86 spec) is the same for both versions or never read by the game.

spirv/ starts as hard links to the current one: translate.py renames each file it writes over the old link, so the
played version's files stay as they were, and the programs whose output did not change cost nothing (a copy where the
file system has no hard links). Not run on its own.
"""

import json
import os
import shutil
import sys
import time
from pathlib import Path

NEXT = "setup-next"
STATE = "setup-state.json"
OUTPUTS = ("mkwgame.so", "mkwgame.dll", "mkwgame.dll.pdb", "mkwgame.id", "pipeline-pairs.bin", "spirv")  # with STATE
# the warm step's (prepare.warm): copies, not links, since the runtime appends to the keys and rewrites the cache; the
# mark is the prewarm's guard against a driver that dies in it (vk_pipe_keys.c prewarm_mark)
WARMED = ("pipeline-cache.bin", "pipeline-keys.bin", "pipeline-keys.prewarm")
PROMOTED = (*OUTPUTS, *WARMED, STATE)
JOURNAL = "promotion.json"


def next_dir(game: Path) -> Path:
    return game / NEXT


def lower_priority() -> None:
    """This process and the tools it starts below normal priority (Windows: its priority class, inherited; elsewhere a
    nice of 10). Best effort: a system that refuses leaves the setup as it was."""
    try:
        if os.name == "nt":
            import ctypes  # noqa: PLC0415

            k32 = ctypes.windll.kernel32
            k32.SetPriorityClass(k32.GetCurrentProcess(), 0x4000)  # BELOW_NORMAL_PRIORITY_CLASS
        else:
            os.nice(10)
    except (OSError, AttributeError) as e:
        print(f"setup: priority not lowered: {e}", file=sys.stderr)


def seed(game: Path) -> Path:
    """setup-next/ with copies of the played version's caches; returns the folder. Its spirv/ is translate.py's (--base
    links the played one's outputs program by program); with no shaders step there is none, and promote keeps the
    played one."""
    out = next_dir(game)
    out.mkdir(parents=True, exist_ok=True)
    for name in WARMED:
        if (game / name).is_file() and not (out / name).exists():
            shutil.copy2(game / name, out / name)
    return out


def load(game: Path, state: dict) -> dict:
    """The state an update in setup-next/ resumes from: its own when an earlier --next run left one, else a copy of the
    played version's (its finished steps are this one's starting point)."""
    p = next_dir(game) / STATE
    try:
        s = json.loads(p.read_text())
    except OSError, ValueError:
        return json.loads(json.dumps(state))
    return s if s.get("version") == state.get("version", s.get("version")) else json.loads(json.dumps(state))


def restore(game: Path, out: Path, names: list[str]) -> None:
    for name in reversed(names):
        src, dst, old = out / name, game / name, game / (name + ".old")
        if not src.exists() and dst.exists():
            dst.rename(src)
        if old.exists():
            if dst.exists(follow_symlinks=False):
                raise FileExistsError(f"cannot restore {old}: {dst} is occupied")
            old.rename(dst)


def recover(game: Path) -> None:
    """Restore an interrupted promotion before setup reads or writes either version's state."""
    out = next_dir(game)
    journal = out / JOURNAL
    if journal.is_file():
        try:
            names = json.loads(journal.read_text())
        except ValueError as e:
            raise OSError(f"invalid promotion journal: {journal}") from e
        if not isinstance(names, list) or names != [name for name in PROMOTED if name in names]:
            raise OSError(f"invalid promotion journal: {journal}")
        restore(game, out, names)
        journal.unlink()


def promote(game: Path, port: str) -> bool:
    """The outputs of a finished --next run for this port version moved into place (the old ones set aside); an
    unfinished or other version's setup-next/ is removed. True when something was promoted."""
    out, t0 = next_dir(game), time.monotonic()
    if not out.is_dir():
        return False
    recover(game)
    try:
        s = json.loads((out / STATE).read_text())
    except OSError, ValueError:
        s = {}
    if not s.get("ready") or (port and s.get("port") != port):
        print(f"setup: {out} is not a finished setup of {port or 'this version'}: removed", file=sys.stderr)
        shutil.rmtree(out, ignore_errors=True)
        return False
    names = [name for name in PROMOTED if (out / name).exists()]
    for name in names:
        old = game / (name + ".old")
        if old.is_dir():
            shutil.rmtree(old)
        else:
            old.unlink(missing_ok=True)
    # Record the planned moves before the first rename. Recovery infers progress from the three paths, including
    # an interruption between moving the played output aside and installing its replacement.
    journal = out / JOURNAL
    pending = journal.with_suffix(".tmp")
    with pending.open("w") as f:
        json.dump(names, f)
        f.flush()
        os.fsync(f.fileno())
    pending.replace(journal)
    try:
        for name in names:
            src, dst = out / name, game / name
            if dst.exists():
                dst.rename(game / (name + ".old"))
            src.rename(dst)
        journal.unlink()
    except OSError as error:
        try:
            recover(game)
        except OSError as rollback:
            raise OSError(f"{error}. Rollback failed ({rollback}). Recovery journal: {journal}") from error
        raise
    shutil.rmtree(out, ignore_errors=True)
    print(f"setup: the update set up beside the played version ({port}) is now in place", file=sys.stderr)
    print(f"time: promote {time.monotonic() - t0:.1f} s", file=sys.stderr)  # the player waits for it (swap run)
    return True


def remove_old(game: Path) -> None:
    """The played version's outputs promote set aside (<name>.old). Apart from promote: the old spirv/'s 46 297 files
    took 10 s to delete on the Windows PC (18/11/2026), and prepare.py deletes them beside the swap's other steps."""
    for name in PROMOTED:
        old = game / (name + ".old")
        shutil.rmtree(old, ignore_errors=True) if old.is_dir() else old.unlink(missing_ok=True)
