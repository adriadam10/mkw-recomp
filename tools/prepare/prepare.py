"""Set the game up on the player's machine from their own copy: check it, copy it, build the game code, translate the
shaders into the player's chosen installation folder.

Why: nothing of the game ships with the port. The start screen (runtime/ui/launcher/launcher.cpp) runs this with the Python of
the player's package and shows its progress; the same steps run from a source checkout with the system's tools. The
order follows totk-pc-recomp's installer (docs/installer.md, MIT): check before writing, one key per step so a
cancelled or failed run resumes from the last finished step.

Steps, into --game (by default the folder of mkw / mkw.exe):
  check     exefs/main is a version of MKW the port knows (NSO build id, recomp/versions.py: 1.0.0, 1.8.0), the RomFS
            has at least 95 % of that version's files (1.0.0: 205 660)
  exefs     exefs/ copied (40 MB)
  romfs     romfs/ copied (23 GB), or --romfs link: its path written to mkw.ini (romfs = ...)
  generate  recomp/gen.py: the game code as C, in <game>/setup-work
  compile   that C with the package's zig (or clang) into mkwgame.so / mkwgame.dll
  shaders   the SM86 spec built here from NVIDIA's nvdisasm (sm86/spec/spec.py, into sm86/; never shipped), then
            tools/shaders/extract.py and translate.py into spirv/
  warm      mkw --prewarm, without the game: the shader libraries of every program pair of the copy into the driver's
            pipeline cache for the first start
  cleanup   setup-work removed but its obj/, zig-cache/ and shaders/: the next update compiles only the chunks whose C
            changed (gamecode.py), zig does not build its libc again, and extract.py writes only new programs

The copy is loose exefs/ and romfs/ folders, or a .nup package (the Switch 2 dump format, nup.py): --source names the
package file itself. From a package, exefs/ is extracted, romfs/ extracted (copy) or the package itself
linked (the runtime reads its RomFS in place).

Output: one event per line on stdout, for the start screen: "step <name>", "progress <done> <total>", "skip <name>",
"done", "error <code> <detail>". Exit codes: 0 done, 1 failed, 2 bad command line. The start screen shows its own
translated text per code with the detail inside: source (no earlier setup to repeat), moved (the last setup's folder,
gone: moved or renamed, #188; a copy's game folder stands in for it, #501), nogame (the folder, without exefs/main and romfs/), encrypted (exefs/main, not an NSO), version (main's build id), romfs (its file count), failed
(a tool's English message) and io (the system's); logs/setup.log gets the English message whole.

Usage: python prepare.py --source DIR|FILE.nup --game DIR [--runtime DIR] [--romfs copy|link] [--jobs N]
                         [--target linux|windows] [--next --port VER]
Without --source (the launcher after an update, tools/prepare/update.py): the copy and the RomFS mode of the last
setup in --game, from its setup-state.json; only the steps whose key changed run again.
--next (an update while the player keeps playing): the outputs the game reads go to <game>/setup-next/, warm's caches
too, and cleanup waits; the next run without --next of that --port version moves them into place first (nextsetup.py).
"""

import faulthandler
import sys
from pathlib import Path


STALL_S = 120


def _early_log() -> bool:
    """logs/setup.log opened before anything else runs, and after two minutes without an event the Python stack of
    each thread written to it: a setup that sat at 0 % CPU before writing a line left no log at all, so nothing said
    where it waited (#251, Windows, 0.1.10). Each event restarts the wait (machine.emit). Rewritten at every setup;
    main() keeps writing to the same file. True when the log is open."""
    try:
        game = Path(sys.argv[sys.argv.index("--game") + 1]).absolute()  # as main()'s, so it keeps this file
        (game / "logs").mkdir(parents=True, exist_ok=True)
        sys.stderr = open(game / "logs/setup.log", "w", encoding="utf-8", buffering=1)  # noqa: SIM115
    except ValueError, IndexError, OSError:
        return False
    print(f"setup: started, Python {sys.version.split()[0]}, {sys.executable}", file=sys.stderr)
    faulthandler.dump_traceback_later(STALL_S, repeat=True, file=sys.stderr)
    return True


_LOGGED = _early_log()

import argparse  # noqa: E402
import hashlib  # noqa: E402
import json
import os
import shutil
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]  # the repository, or the package's setup/src
sys.path.insert(0, str(ROOT / "tools/build"))
sys.path.insert(0, str(ROOT / "tools/i18n"))
import build  # (GEN_FLAGS, module_glue, game_id: the same game code as build.py --dist)
import game_texts  # (build_all: the game's texts in ca, gl, eu, tr)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamecode import DamagedSource, compile_game, cpu_level
from gamesource import check, is_nx, loose_romfs, unpack
import seqnodes  # (the phase table Straight to presses through the menus by)
from gamecopy import BUILD_BYTES, check_space, clear_dir, copy_tree, own_dir, remove_stale, romfs_bytes, tree_bytes
from nup import Nup
from machine import (
    ZIG_CACHE,
    Failed,
    default_cc,
    default_glslc,
    emit,
    hold_lock,
    mem_available_gb,
    mem_gb,
    children_cpu,
    own_children,
    run,
    shader_jobs,
)
import machine
import nextsetup
from beside import Beside
from shaderstep import (  # noqa: F401  (spec_command: tests/launcher/test_setup.py)
    PACK_VERSION,
    PAIRS_VERSION,
    ensure_pack,
    ensure_pairs,
    shaders,
    shipped,
    spec_command,
    translator_key,
)
import update  # (ca_file)

PREP_VERSION = 1  # bump when a step's output changes meaning: every step runs again
# (build.py put recomp/ on the path)
from versions import Version

if _LOGGED:
    machine.STALL_S = STALL_S  # every event restarts the stack dump's wait (machine.emit)

SHADER_DIRS = ("Effect", "Layout", "Lib")  # extract.py's SHADER_DIRS: with Shader/, the RomFS folders it reads
STEPS = ("check", "exefs", "romfs", "generate", "compile", "shaders", "warm", "cleanup")
WARM_TIMEOUT = 1800  # mkw --prewarm: ~10 000 shader libraries compiled cold on every core


def ini_set(ini: Path, key: str, value: str) -> None:
    """key = value in mkw.ini (runtime/core/base/cfg.c format), replacing a line with that key."""
    lines = ini.read_text(encoding="utf-8").splitlines() if ini.is_file() else []
    norm = key.upper().removeprefix("MKW_")
    keep = [ln for ln in lines if ln.split("=", 1)[0].strip().upper().removeprefix("MKW_") != norm]
    ini.write_text("\n".join([*keep, f"{key} = {value}"]) + "\n", encoding="utf-8")


def ini_get(ini: Path, key: str) -> str:
    """key's value in mkw.ini, "" when it has none."""
    norm = key.upper().removeprefix("MKW_")
    for ln in ini.read_text(encoding="utf-8").splitlines() if ini.is_file() else []:
        k, sep, v = ln.partition("=")
        if sep and k.strip().upper().removeprefix("MKW_") == norm:
            return v.strip()
    return ""


def code_mods(game: Path) -> tuple[list[dict], str]:
    """The enabled mods' code bytes (mod.json "image", tools/mod/ips.py), each with its mod's name, for gen.py --image;
    and a key over them and every installed mod's switchable "patches" (gen.py compiles those in, recomp/patches.py
    load_mods): "" without any, so a setup without code mods keeps its keys."""
    enabled = [m.strip() for m in ini_get(game / "mkw.ini", "mods").split(",") if m.strip()]
    image, switchable = [], {}
    mods = game / "mods"
    for mj in sorted(mods.glob("*/mod.json")) if mods.is_dir() else []:
        try:
            meta = json.loads(mj.read_text(encoding="utf-8"))
        except OSError, ValueError:
            continue
        if meta.get("patches"):
            switchable[mj.parent.name] = meta["patches"]
        if mj.parent.name in enabled:
            image += [{"mod": mj.parent.name, **p} for p in meta.get("image") or []]
    if not image and not switchable:
        return [], ""
    blob = json.dumps([image, switchable], sort_keys=True).encode()
    return image, hashlib.sha256(blob).hexdigest()[:16]



def generate(exefs: Path, work: Path, jobs: int, v: Version, image: list[dict]) -> None:
    gen = work / "generated"
    cmd = [sys.executable, str(ROOT / "recomp/gen.py"), "--exefs", str(exefs), "--out", str(gen), "--jobs", str(jobs),
           "--patches", str(v.patches), "--mods", str(exefs.parent / "mods")]  # fmt: skip
    if image:  # the enabled code mods' bytes, lifted in place
        (work / "image.json").write_text(json.dumps(image), encoding="utf-8")
        cmd += ["--image", str(work / "image.json")]
    run(cmd)


def code_key(files: list[Path]) -> str:
    """Generate's and compile's keys besides the ExeFS and game_id: the code that makes their output. Before, they were
    game_id alone, so an update that changed only recomp/ (or the glue and flags) kept the old game code."""
    h = hashlib.sha256()
    for f in shipped(files):
        h.update(f.read_bytes())
    return h.hexdigest()[:16]


def migrate_keys(state: dict, keys: dict[str, str], before: dict[str, str]) -> None:
    """A step done under its key from before 0.1.10 (with the source folders, `before`) is taken as done under the new
    one: else the first update after the change would set up everything again."""
    for step, old in before.items():
        if state["done"].get(step) == old:
            state["done"][step] = keys[step]


def regenerate_if_cleaned(state: dict, keys: dict[str, str], work: Path) -> None:
    """Undoes generate when compile must run and cleanup already removed generated/: a compile whose key changed alone
    (an update that touched only tools/build/build.py, as the first after 0.1.9) stopped on the missing layout.json."""
    if state["done"].get("compile") != keys["compile"] and not (work / "generated/layout.json").is_file():
        state["done"].pop("generate", None)


def redo_exefs_if_missing(state: dict, game: Path) -> None:
    """Undoes the exefs step when its copy is gone: the step is skipped by its key (source and version), and a folder
    emptied or moved since made generate stop on "module main not in the layout" (#419), which names no cause."""
    if "exefs" in state["done"] and not (game / "exefs/main").is_file():
        state["done"].pop("exefs")


GENERATOR = [
    *sorted((ROOT / "recomp").glob("*.py")),
    *sorted((ROOT / "recomp").glob("patches-mkw*.txt")),
]  # each version's table
COMPILER = [*GENERATOR, ROOT / "tools/build/build.py", ROOT / "tools/prepare/gamecode.py"]


def warm(game: Path, work: Path, runtime: Path | None = None, out: Path | None = None) -> None:
    """mkw --prewarm, without the game: the shader libraries of every program pair of the copy (pipeline-pairs.bin,
    from the shaders step) compiled into the driver's caches and pipeline-cache.bin, so the first start only loads
    them (runtime/gfx/vk/pipe/vk_pipe_pairs.c). Until 17/11/2026 this ran the game for a minute and kept the keys of
    what its title drew; a new place still drew with libraries compiling. Its log is logs/warm.log (#437: in mkw.log
    it pushed the player's last run out of the file reports attach). A driver that crashes or hangs here does not fail
    the setup: the prewarm's mark (pipeline-keys.prewarm) then skips it at every start on that GPU and driver, and the
    game builds its pipelines as it draws, as before. out: an update's setup-next/ (--next), whose SPIR-V it reads
    and where its caches go; the game folder's mkw.ini still picks the GPU."""
    exe = (runtime or game) / ("mkw.exe" if os.name == "nt" else "mkw")
    env = dict(os.environ, MKW_LOG_NAME="warm", MKW_GAME_DIR=str(game))
    if out:  # its own spirv/ when the update translated (nextsetup.seed)
        spirv = out / "spirv" if (out / "spirv").is_dir() else game / "spirv"
        env |= {"MKW_SPIRV": str(spirv), "MKW_PIPELINE_CACHE": str(out / "pipeline-cache.bin")}
    emit("progress", 0, 1)
    try:
        p = subprocess.run([str(exe), "--prewarm"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=WARM_TIMEOUT, check=False)  # fmt: skip
    except subprocess.TimeoutExpired:
        print(f"warm: the pipeline prewarm took over {WARM_TIMEOUT} s; left to the game", file=sys.stderr)
    else:
        if p.returncode:
            print(f"warm: the pipeline prewarm failed (exit {p.returncode}, {game / 'logs/warm.log'}); left to the game",
                  file=sys.stderr)  # fmt: skip
    emit("progress", 1, 1)


def clear_work(work: Path) -> None:
    """setup-work emptied but its obj/ and zig-cache/ (the next update compiles only the chunks whose C changed) and
    shaders/ (extract.py's output, named by content: the next update's extract.py writes only new programs, and
    nothing deletes its ~23 000 files, 159 MB)."""
    for p in work.glob("*"):
        if p.is_dir() and p.name not in ("obj", ZIG_CACHE, "shaders"):
            shutil.rmtree(p, ignore_errors=True)


def load_state(game: Path) -> dict:
    p = game / "setup-state.json"
    try:
        s = json.loads(p.read_text())
    except OSError, ValueError:
        return {"done": {}}
    return s if s.get("version") == PREP_VERSION else {"done": {}}


def save_state(game: Path, state: dict) -> None:
    state["version"] = PREP_VERSION
    p = game / "setup-state.json"
    p.with_suffix(".tmp").write_text(json.dumps(state, indent=1))
    p.with_suffix(".tmp").replace(p)


def main() -> None:
    # This Python runs with -I, but the tools it starts (gen.py, spec.py, translate.py and their workers) would take the
    # player's PYTHONPATH or PYTHONHOME and load another Python's modules: "Module use of python312.dll conflicts" (#177)
    for k in [k for k in os.environ if k.startswith("PYTHON")]:
        del os.environ[k]
    # numpy's OpenBLAS sets a buffer aside per hardware thread in every process that imports it, and Windows counts it
    # as private memory: 178 MB on 6 threads against 17 MB with one (18/11/2026), ~0.65 GB a generate worker on 24.
    # No tool does linear algebra, and its workers run one per core already
    os.environ["OPENBLAS_NUM_THREADS"] = "1"
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--source",
        type=Path,
        help="the folder with the player's exefs/ and romfs/, or their .nup package (default: the last's)",
    )
    ap.add_argument("--game", type=Path, required=True, help="the game installation folder")
    ap.add_argument("--runtime", type=Path, help="the folder of mkw / mkw.exe (default: --game)")
    ap.add_argument("--romfs", choices=("copy", "link"), help="(default: the last setup's, else copy)")
    ap.add_argument("--jobs", type=int, default=0, help="parallel jobs (default: from cores and memory)")
    ap.add_argument("--cc", help="the C compiler command (default: the package's zig cc, else clang)")
    ap.add_argument("--glslc", help="(default: the package's, else glslc on PATH)")
    ap.add_argument("--target", choices=("linux", "windows"), default="windows" if os.name == "nt" else "linux")
    ap.add_argument(
        "--next",
        action="store_true",
        help="an update set up beside the played version, in setup-next/ (no warm step): its next run swaps it in",
    )
    ap.add_argument(
        "--port", default="", help="the port version this setup is for (the start screen's): kept in the state"
    )
    args = ap.parse_args()
    own_children()
    if ca := update.ca_file():  # spec.py downloads NVIDIA's redistributables with this Python (issue #102, NixOS)
        os.environ["SSL_CERT_FILE"] = ca
    # Keep the launcher's accepted spelling: resolving a short symlink/junction can expand beyond the runtime's
    # directory limit, making warm-up reject the very installation this setup just built.
    game = args.game.absolute()
    runtime = args.runtime.resolve() if args.runtime else game
    lock = hold_lock(game)  # noqa: F841 (held until exit)
    try:
        if args.next:  # the player may be racing meanwhile: the game's threads go first; the tools inherit it
            nextsetup.lower_priority()
            nextsetup.recover(game)
        else:  # the update an earlier --next run set up beside the played version, now that this one runs
            nextsetup.promote(game, args.port)
    except OSError as e:
        emit("error", "io", e)
        sys.exit(1)
    # the played version's outputs promote set aside, deleted beside this run's steps (joined at its end)
    removing = Beside("remove-old", lambda _: nextsetup.remove_old(game)) if not args.next else None
    state = load_state(game)
    dest = game  # where the outputs the game reads, and the state, are written
    if args.next:  # beside the played version's, the state with them (nextsetup.py)
        state, dest = nextsetup.load(game, state), nextsetup.next_dir(game)
    if not args.source:  # the last setup's; before 0.1.5 the state kept them only in its keys
        done_exefs = Path(state["done"]["exefs"]) if "exefs" in state["done"] else None
        last = state.get("source") or (
            str(done_exefs if done_exefs.is_file() else done_exefs.parent) if done_exefs else ""
        )
        if not last:
            emit("error", "source", "no earlier setup in this folder")
            sys.exit(2)
        args.source = Path(last)
        args.romfs = args.romfs or state.get("romfs_mode") or state["done"].get("romfs", "copy").split(" ", 1)[0]
        if not args.source.exists():
            # a copy keeps the whole game in this folder: once the player deleted the original files, this folder is
            # the source (#501: "choose it again" was impossible after an update, and choosing the game folder worked)
            if args.romfs == "copy" and (game / "exefs/main").is_file() and own_dir(game / "romfs", Path(last)):
                print(f"setup: {last} is gone; the copy in {game} is the source", file=sys.stderr)
                args.source = game
            else:
                emit("error", "moved", last)
                sys.exit(1)
    args.romfs = args.romfs or "copy"
    src = args.source.resolve()
    state["source"], state["romfs_mode"] = str(src), args.romfs
    # The start screen reads only stdout: the tools' output and the timings go to logs/setup.log, the file a report
    # attaches (issue #2 had none to give).
    (game / "logs").mkdir(parents=True, exist_ok=True)
    if getattr(sys.stderr, "name", "") != str(game / "logs/setup.log"):  # _early_log opened it unless --game was odd
        sys.stderr = open(game / "logs/setup.log", "w", encoding="utf-8", buffering=1)  # noqa: SIM115
    print(f"setup: {' '.join(sys.argv)}", file=sys.stderr)

    # Within 60 % of the RAM, by what one job was measured to take (13/11/2026, 12 at once): a compile 0.5 GB (peaks
    # 266-409 MB, build.py --job-mem-gb), a generate worker 0.7 GB (gen.py: 5 065 MB for 12, one up to 608 MB). 1 GB for
    # both kept a 12-thread PC with 13 GB at 8 compiles and an 8 GB one at 4.
    # Windows, 15/11/2026 (24 threads, 31 GB): 24 generate workers held 26.6 GB of private memory, 1.1 GB each (cause
    # not measured), and took the free memory from 8.9 GB to 0.13 GB; 24 compiles held 7.6 GB, 0.32 GB each. So a
    # Windows worker counts 1.2 GB (the 1.1 measured plus a margin, as 0.7 is for 0.42 on Linux). The budget is also
    # what is free when the setup starts (the player's browser and launcher stay open), as shader_jobs does on every
    # system; on Linux MemAvailable counts the reclaimable cache, and it lowers the jobs only when less than 60 % of the
    # RAM is available (not measured on Linux).
    # 18/11/2026, the Windows PC (--next, 3 workers): 374 MB each at most with OPENBLAS_NUM_THREADS=1 (numpy's OpenBLAS
    # had reserved memory for every thread in each worker), so Windows counts 0.7 GB as Linux does.
    free = mem_available_gb()
    # --next: the game may start while this runs, and low priority leaves it the CPU but not the memory; half of it
    share = 0.5 if args.next else 1.0
    budget = min(mem_gb() * 0.6, free) * share

    def fit(gb: float) -> int:
        return args.jobs or max(1, min(os.cpu_count() or 1, int(budget / gb)))

    jobs, gen_jobs = fit(0.5), fit(0.7)
    # setup.log only (stderr): what the machine gave the setup, and below each step's time, so two setups (Linux and
    # Windows, or a player's and ours) compare step by step (Adrià 13/11/2026: "los logs de preparación deberían
    # guardar tiempos"; 0.1.10's Windows setup took twice the Linux one and nothing said where)
    print(
        f"machine: {os.cpu_count()} threads, {mem_gb():.1f} GB, {free:.1f} GB free, {jobs} compile and {gen_jobs} "
        f"generate jobs, {sys.platform}",
        file=sys.stderr,
    )
    t_all, step, t_step = time.monotonic(), "check", time.monotonic()
    glslc = args.glslc or default_glslc(game)
    work = game / "setup-work"
    try:
        emit("step", "check")
        cc = args.cc.split() if args.cc else default_cc(game, args.target)
        exefs_src, romfs_src, version = check(src, game, runtime)
        romfs = game / "romfs" if args.romfs == "copy" else romfs_src
        # The version (from exefs/main's build id) says which game a step built from; the folder only matters to the
        # steps that copy from it or point mkw.ini at it. With the folder in every key, moving or renaming it redid
        # generate, compile and the shaders (#188: 16 min on Linux, 31 on Windows)
        # code mods: their key joins generate's and compile's only when there are some (a mod enabled, disabled,
        # imported or deleted builds the game code again; tools/mod/ips.py)
        image, mods_key = code_mods(game)
        with_mods = f" mods {mods_key}" if mods_key else ""
        keys = {
            "exefs": str(exefs_src),
            "romfs": f"copy {version.name}" if args.romfs == "copy" else f"link {romfs_src}",
            "generate": f"{build.game_id()} {version.name} {code_key(GENERATOR)}{with_mods}",
            # a level only when it is not the default (gamecode.py cpu_level): v3 keeps the key it had without it
            "compile": f"{build.game_id()} {version.name} {args.target} {code_key(COMPILER)}{with_mods}"
            + ("" if cpu_level() == "v3" else f" {cpu_level()}"),
            "shaders": f"{version.name} {translator_key()}",
            # with the shaders' key: the prewarm builds the libraries of this SPIR-V (an update that changed only the
            # translator skipped it and left the new libraries to the first start)
            "warm": f"{build.game_id()} {version.name} {translator_key()} pairs{PAIRS_VERSION} pack{PACK_VERSION}",
        }
        migrate_keys(state, keys, {
            "romfs": f"{args.romfs} {romfs_src}",
            "generate": f"{build.game_id()} {exefs_src} {version.name} {code_key(GENERATOR)}",
            "compile": f"{build.game_id()} {exefs_src} {version.name} {args.target} {code_key(COMPILER)}",
            "shaders": f"{exefs_src} {romfs_src} {translator_key()}",
            "warm": f"{build.game_id()} {romfs_src}",
        })  # fmt: skip
        regenerate_if_cleaned(state, keys, work)
        redo_exefs_if_missing(state, game)
        # The room the steps to come take: the RomFS copy less what a cut one left, and the game's build the first
        # time (an update writes over its files instead of beside them)
        need = 0
        if "romfs" in STEPS and args.romfs == "copy" and state["done"].get("romfs") != keys["romfs"]:
            need += max(romfs_bytes(romfs_src) - tree_bytes(romfs), 0)
        if "compile" in STEPS and "compile" not in state["done"]:
            need += BUILD_BYTES
        check_space(game, need)
        # setup-next/'s cache copies and the shaders step go beside generate and compile
        # (beside.py): the shaders start with generate, translate.py waits for its end
        seeding = Beside("seed", lambda _: nextsetup.seed(game)) if args.next else None
        code_ready, beside, tidying = threading.Event(), None, None

        def shaders_job(b: Beside) -> None:
            def before_translate() -> None:
                code_ready.wait()
                if seeding:
                    seeding.join()

            # extract.py reads a .nup itself; a .nx's shader archives are unpacked first
            src_romfs = loose_romfs(runtime, romfs, work, "Shader", *SHADER_DIRS) if is_nx(romfs) else romfs
            shaders(game / "exefs", src_romfs, game, work, glslc, lambda: args.jobs or shader_jobs(share), cc, dest,
                    before_translate, b.progress)  # fmt: skip

        for step in STEPS[1:]:
            if step == "generate" and state["done"].get("shaders") != keys["shaders"]:
                beside = Beside("shaders", shaders_job)
            # generate done or skipped: translate.py may take the cores, beside compile when an earlier setup left its
            # objects (an update, a few chunks); a first setup's 360 compiles and translate.py would both size their
            # jobs by the same free memory. translate.py beside generate too gained nothing on the 6-thread Windows
            # PC (344 and 346 s against 334, up-win-h1e/h1f, 18/11/2026): the two only share the same cores
            if step == "compile" and "compile" in state["done"]:
                code_ready.set()
            if step == "shaders":
                code_ready.set()
            if step == "warm" and seeding:
                seeding.join()
            if step == "warm" and tidying:  # setup-work/pack may be unpacked again for ensure_pairs (a .nx copy)
                tidying.join()
            # what cleanup removes, here beside translate.py once compile is done: the swap's cleanup spent 12-17 s
            # in it at the player's first start on the Windows PC, and beside warm it slowed that by 11 s (18/11/2026);
            # generated/ comes back if a compile needs it (regenerate_if_cleaned)
            if step == "shaders" and args.next:
                tidying = Beside("clear-work", lambda _: clear_work(work))
            # cleanup after the swap, by the new version's own run; warm here, into setup-next/: the Windows PC's
            # swap run spent 106 s in it at the player's first start (18/11/2026)
            if args.next and step == "cleanup":
                continue
            # The start screen sets up again when its version differs (launcher.cpp): an update by hand (a new
            # AppImage or zip over the old) kept the old steps (#50: 0.1.8 played with 0.1.7's shaders). Kept once the
            # game is built, before the optional warm-up: a warm-up that keeps failing (#50's hang) must not repeat
            # the setup at every start.
            if step == "warm" and args.port:
                state["port"] = args.port
                save_state(dest, state)
            key = keys.get(step, "")
            if step != "cleanup" and state["done"].get(step) == key:
                emit("skip", step)
                continue
            emit("step", step)
            t_step, c_step = time.monotonic(), children_cpu()
            if step == "exefs":
                if is_nx(exefs_src):
                    clear_dir(game / "exefs", exefs_src)
                    unpack(runtime, exefs_src, game, "exefs")
                elif exefs_src.is_file():
                    nup = Nup(exefs_src)
                    nup.extract("exefs", game / "exefs", lambda d, t: emit("progress", d, t))
                    remove_stale(game / "exefs", exefs_src, set(nup.files("exefs")), version.name)
                else:
                    copy_tree(exefs_src, game / "exefs", sum(1 for p in exefs_src.iterdir() if p.is_file()),
                              version.name)
            elif step == "romfs":
                if args.romfs == "copy" and is_nx(romfs_src):
                    clear_dir(romfs, romfs_src)
                    unpack(runtime, romfs_src, game, "romfs")
                    ini_set(game / "mkw.ini", "romfs", "romfs")
                elif args.romfs == "copy" and romfs_src.is_file():
                    nup = Nup(romfs_src)
                    nup.extract("romfs", romfs, lambda d, t: emit("progress", d, t))
                    remove_stale(romfs, romfs_src, set(nup.files("romfs")), version.name)
                    ini_set(game / "mkw.ini", "romfs", "romfs")
                elif args.romfs == "copy":
                    previous = state.get("romfs_copy_in_progress", state["done"].get("romfs", key))
                    verify_existing = previous != key
                    # An interrupted version switch can leave files from both versions. Persist that before
                    # writing any files so even a switch back checks their bytes instead of skipping the copy.
                    state["romfs_copy_in_progress"] = None if verify_existing else key
                    state["done"].pop("romfs", None)
                    save_state(game, state)
                    copy_tree(romfs_src, romfs, version.romfs_files, version.name, verify_existing=verify_existing)
                    ini_set(game / "mkw.ini", "romfs", "romfs")
                    del state["romfs_copy_in_progress"]
                else:
                    ini_set(game / "mkw.ini", "romfs", str(romfs_src))
            elif step == "generate":
                generate(game / "exefs", work, gen_jobs, version, image)
            elif step == "compile":
                try:
                    compile_game(game, game / "exefs", work, cc, args.target, jobs, dest)
                except DamagedSource as e:  # once: a second damaged write is the disk or the memory, not chance
                    print(f"{e}: generating again", file=sys.stderr)
                    emit("step", "generate")
                    generate(game / "exefs", work, gen_jobs, version, image)
                    emit("step", "compile")
                    compile_game(game, game / "exefs", work, cc, args.target, jobs, dest)
            elif step == "shaders":
                beside.join(show=True)
            elif step == "warm":
                src_romfs = loose_romfs(runtime, romfs, work, "Shader", *SHADER_DIRS) if is_nx(romfs) else romfs
                ensure_pairs(game / "exefs", src_romfs, work, dest)
                ensure_pack(dest / "spirv")
                warm(game, work, runtime, dest if args.next else None)
            elif step == "cleanup":
                clear_work(work)
                # the languages the game does not ship (ca, gl, eu, tr), from this version's translations (the step runs
                # at every setup, so an update brings its new texts); about a second, ~25 s the first time Turkish composes its fonts
                game_texts.build_all(game, loose_romfs(runtime, romfs, work, "Mals", "Font"))
                # Settings > Hacks > Straight to waits for the game's phases, named by this table (seqnodes.py); a
                # setup without it left every press of it unfired. About a second, at every setup as above.
                try:  # optional: a game folder it cannot write leaves only Straight to without its phases
                    seqnodes.write(game / "exefs", game / f"seq_nodes-{version.number}.tsv")
                except (OSError, ValueError) as e:
                    print(f"warning: seq_nodes table not written: {e}", file=sys.stderr)
                shutil.rmtree(work / "pack", ignore_errors=True)
            state["done"][step] = key
            save_state(dest, state)
            cpu = children_cpu() - c_step  # the CPU of its tools: whether a step leaves cores idle (H1, build/updates)
            print(f"time: {step} {time.monotonic() - t_step:.1f} s, cpu {cpu:.1f} s", file=sys.stderr)
        for b in (seeding, removing, tidying):
            if b:
                b.join()
        print(f"time: setup {time.monotonic() - t_all:.1f} s", file=sys.stderr)
        if args.next:  # finished: the next start of the new version moves it into place (nextsetup.promote)
            state["port"], state["ready"] = args.port, True
            save_state(dest, state)
        emit("done")
    except Failed as e:
        print(f"time: {step} failed after {time.monotonic() - t_step:.1f} s", file=sys.stderr)
        print(f"failed: {e}", file=sys.stderr)
        emit("error", e.code, e.detail)
        sys.exit(1)
    except OSError as e:
        emit("error", "io", e)
        sys.exit(1)


if __name__ == "__main__":
    main()
