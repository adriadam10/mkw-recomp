"""Set the game up on the player's machine from their own copy: check it, copy it, build the game code, translate the
shaders and write the 720p resolution overlay, into the folder of the executable.

Why: nothing of the game ships with the port. The start screen (runtime/ui/launcher/launcher.cpp) runs this with the Python of
the player's package and shows its progress; the same steps run from a source checkout with the system's tools. The
order follows totk-pc-recomp's installer (docs/installer.md, MIT): check before writing, one key per step so a
cancelled or failed run resumes from the last finished step.

Steps, into --game (the folder of mkw / mkw.exe):
  check     exefs/main is a version of MKW the port knows (NSO build id, recomp/versions.py: 1.0.0, 1.8.0), the RomFS
            has at least 95 % of that version's files (1.0.0: 205 660)
  exefs     exefs/ copied (40 MB)
  romfs     romfs/ copied (23 GB), or --romfs link: its path written to mkw.ini (romfs = ...)
  generate  recomp/gen.py: the game code as C, in <game>/setup-work
  compile   that C with the package's zig (or clang) into mkwgame.so / mkwgame.dll
  shaders   the SM86 spec built here from NVIDIA's nvdisasm (sm86/spec/spec.py, into sm86/; never shipped), then
            tools/shaders/extract.py and translate.py into spirv/
  warm      the game run once for a minute: the driver's pipeline cache and the pipeline keys for the first start
  cleanup   setup-work removed but its obj/: the next update compiles only the chunks whose C changed (gamecode.py)

Output: one event per line on stdout, for the start screen: "step <name>", "progress <done> <total>", "skip <name>",
"done", "error <code> <detail>". Exit codes: 0 done, 1 failed, 2 bad command line. The start screen shows its own
translated text per code with the detail inside: source (no earlier setup to repeat), nogame (the folder, without
exefs/main and romfs/), encrypted (exefs/main, not an NSO), version (main's build id), romfs (its file count), failed
(a tool's English message) and io (the system's); logs/setup.log gets the English message whole.

Usage: python prepare.py --source DIR --game DIR [--romfs copy|link] [--jobs N] [--cc CMD] [--glslc PATH]
                         [--target linux|windows]
Without --source (the launcher after an update, tools/prepare/update.py): the copy and the RomFS mode of the last
setup in --game, from its setup-state.json; only the steps whose key changed run again.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # the repository, or the package's setup/src
sys.path.insert(0, str(ROOT / "tools/build"))
sys.path.insert(0, str(ROOT / "tools/i18n"))
import build  # (GEN_FLAGS, module_glue, game_id: the same game code as build.py --dist)
import game_texts  # (build_all: the game's texts in ca, gl, eu)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gamecode import compile_game
from machine import (
    Failed,
    crt_spec,
    default_cc,
    default_glslc,
    emit,
    mem_gb,
    own_children,
    shader_jobs,
    tool_env,
)
import update  # (ca_file)

PREP_VERSION = 1  # bump when a step's output changes meaning: every step runs again
from versions import Version, detect, main_build_id  # (build build.py put recomp/ on the path)
ROMFS_MARGIN = 0.95  # a few short still passes: some dumping tools skip empty files
STEPS = ("check", "exefs", "romfs", "generate", "compile", "shaders", "warm", "cleanup")
WARM_SECONDS = 60  # the driver cache is saved every 20 s and the pipeline keys every 2 (vk_pipeline.c, vk_pipe_keys.c)


# ---- check
def find_game(src: Path) -> tuple[Path, Path]:
    """exefs/ and romfs/ in src or up to two levels below it."""
    for depth in range(3):
        for d in [src, *src.glob("/".join(["*"] * depth))] if depth else [src]:
            if (d / "exefs/main").is_file() and (d / "romfs").is_dir():
                return d / "exefs", d / "romfs"
    raise Failed(f"no exefs/main and romfs/ in {src}", "nogame", str(src))


def count_files(top: Path, enough: int) -> int:
    n = 0
    for _, _, files in os.walk(top):
        n += len(files)
        if n >= enough:
            break
    return n


def check(src: Path) -> tuple[Path, Path, Version]:
    exefs, romfs = find_game(src)
    try:
        got = main_build_id(exefs)
    except ValueError:
        raise Failed(f"{exefs / 'main'} is not an NSO: decrypted exefs needed", "encrypted", str(exefs / "main")) from None
    try:
        v = detect(exefs)
    except ValueError as e:
        raise Failed(str(e), "version", got) from None
    need = int(v.romfs_files * ROMFS_MARGIN)
    n = count_files(romfs, need)
    if n < need:
        raise Failed(f"the RomFS has {n} files, {v.name} has {v.romfs_files}: an incomplete copy", "romfs", str(n))
    emit("version", v.name)
    return exefs, romfs, v


# ---- copies
COPY_JOBS = 6  # files copied at once: NTFS creates ~170 small files a second from one stream, ~460 from six (Windows test PC, 08/11/2026)


def copy_one(s: Path, t: Path) -> None:
    """One file, unless already there with the same size (resume); through .part, so a cut copy is never taken."""
    if t.is_file() and t.stat().st_size == s.stat().st_size:
        return
    part = t.with_name(t.name + ".part")
    shutil.copyfile(s, part)
    part.replace(t)


def copy_tree(src: Path, dst: Path, total: int) -> None:
    """src into dst with progress, COPY_JOBS files at a time: the RomFS's 205 660 files one by one were most of a
    copy's time on Windows, where creating a file costs more than its bytes."""
    done = 0
    with ThreadPoolExecutor(COPY_JOBS) as pool:
        pending = []
        for d, _, files in os.walk(src):
            out = dst / Path(d).relative_to(src)
            out.mkdir(parents=True, exist_ok=True)
            pending += [pool.submit(copy_one, Path(d, f), out / f) for f in files]
        for fut in as_completed(pending):
            fut.result()
            done += 1
            if done % 500 == 0 or done == total:
                emit("progress", done, total)


def ini_set(ini: Path, key: str, value: str) -> None:
    """key = value in mkw.ini (runtime/core/base/cfg.c format), replacing a line with that key."""
    lines = ini.read_text(encoding="utf-8").splitlines() if ini.is_file() else []
    norm = key.upper().removeprefix("MKW_")
    keep = [ln for ln in lines if ln.split("=", 1)[0].strip().upper().removeprefix("MKW_") != norm]
    ini.write_text("\n".join([*keep, f"{key} = {value}"]) + "\n", encoding="utf-8")


# ---- the game code
def run(cmd: list[str], env: dict[str, str] | None = None, progress_prefix: str = "") -> None:
    """A tool's run; its 'progress n total' lines (translate.py) pass through, the rest goes to stderr."""
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    tail = []
    for line in p.stdout:
        if progress_prefix and line.startswith(progress_prefix):
            emit(line.strip())
        else:
            sys.stderr.write(line)
            tail = (tail + [line.strip()])[-5:]
    if p.wait():
        raise Failed(f"{Path(cmd[1] if len(cmd) > 1 else cmd[0]).name} failed: {' / '.join(tail)}")


def generate(exefs: Path, work: Path, jobs: int, v: Version) -> None:
    gen = work / "generated"
    run([sys.executable, str(ROOT / "recomp/gen.py"), "--exefs", str(exefs), "--out", str(gen), "--jobs", str(jobs),
         "--patches", str(v.patches)])  # fmt: skip


def code_key(files: list[Path]) -> str:
    """Generate's and compile's keys besides the ExeFS and game_id: the code that makes their output. Before, they were
    game_id alone, so an update that changed only recomp/ (or the glue and flags) kept the old game code."""
    h = hashlib.sha256()
    for f in files:
        h.update(f.read_bytes())
    return h.hexdigest()[:16]


GENERATOR = [*sorted((ROOT / "recomp").glob("*.py")), *sorted((ROOT / "recomp").glob("patches-mkw*.txt"))]  # each version's table
COMPILER = [*GENERATOR, ROOT / "tools/build/build.py", Path(__file__).with_name("gamecode.py")]


# ---- shaders and the overlay
def translator_key() -> str:
    """The shader step's key: every file the translation's output depends on (the translator, the decoder, extract.py
    and translate.py), as translate.py's translator_hash minus the spec, which this step builds. With translate.py
    alone, an update that changed only sm86/translate kept the old SPIR-V (#26: 0.1.7's translator fixes and the
    position default never reached a set-up game)."""
    h = hashlib.sha256()
    files = sorted((ROOT / "sm86/translate").rglob("*.py")) + sorted((ROOT / "sm86/decode").rglob("*.py"))
    for f in [*files, ROOT / "tools/shaders/extract.py", ROOT / "tools/shaders/translate.py"]:
        h.update(f.read_bytes())
    return h.hexdigest()[:16]


def sm86_spec(game: Path, work: Path, cc: list[str]) -> Path:
    """The spec translate.py decodes with: a source checkout's own build/sm86, else built once into <game>/sm86."""
    for spec in (ROOT / "build/sm86/instructions.json", game / "sm86/instructions.json"):
        if spec.is_file():
            return spec
    crt = crt_spec(game, cc)  # spec.py links a DLL too on Windows: without it, zig builds its libc there (issue #2)
    run(spec_command(game, work, cc, crt))
    return game / "sm86/instructions.json"


def spec_command(game: Path, work: Path, cc: list[str], crt: Path | None) -> list[str]:
    """spec.py's command line; the compiler goes as JSON, whose paths may have spaces (issue #29)."""
    return [sys.executable, str(ROOT / "sm86/spec/spec.py"), "--out", str(game / "sm86"), "--cache",
            str(work / "nvidia"), "--cc", json.dumps(cc), *(["--crt", str(crt)] if crt else [])]  # fmt: skip


def shaders(exefs: Path, romfs: Path, game: Path, work: Path, glslc: str, jobs: int, cc: list[str]) -> None:
    spec = sm86_spec(game, work, cc)
    sh = work / "shaders"
    run([sys.executable, str(ROOT / "tools/shaders/extract.py"), "--romfs", str(romfs), "--exefs", str(exefs),
         "--out", str(sh)])  # fmt: skip
    while True:
        try:
            run([sys.executable, str(ROOT / "tools/shaders/translate.py"), "--shaders", str(sh),
                 "--out", str(game / "spirv"), "--glslc", glslc, "--no-validate", "--progress", "--jobs", str(jobs),
                 "--spec", str(spec)], env=tool_env(glslc), progress_prefix="progress")  # fmt: skip
            break
        except Failed as e:
            # A worker that dies (the system ends it when memory runs out) breaks the whole pool (issue #24,
            # BrokenProcessPool). What is translated stays (translate.py resumes); half the workers need half the memory.
            if "BrokenProcessPool" not in str(e) or jobs == 1:
                raise
            jobs = max(1, jobs // 2)
            print(f"shaders: a worker ended abruptly, again with {jobs} workers", file=sys.stderr)
    for p in (game / "spirv").glob("*.glsl"):  # the runtime reads .spv and .res.json; the GLSL only made them
        p.unlink()


def warm(game: Path, work: Path) -> None:
    """The game run once to its title, so the first real start finds pipeline-cache.bin and pipeline-keys.bin. Its
    saves go to a scratch profile: the player's first start stays a first start. The window opens without taking focus
    and without sound; the run is ended after WARM_SECONDS."""
    exe = game / ("mkw.exe" if os.name == "nt" else "mkw")
    env = dict(os.environ, MKW_LAUNCHED="1", MKW_ASSUME_FOCUS="1", MKW_AUDIO="none",
               MKW_PROFILE_DIR=str(work / "warm-profile"))  # fmt: skip
    p = subprocess.Popen([str(exe)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for t in range(WARM_SECONDS):
        if p.poll() is not None:
            raise Failed(f"the game stopped while warming up (exit {p.returncode}); its log is in {game / 'logs'}")
        emit("progress", t, WARM_SECONDS)
        time.sleep(1)
    p.terminate()
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
    emit("progress", WARM_SECONDS, WARM_SECONDS)


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
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--source", type=Path, help="the folder with the player's exefs/ and romfs/ (default: the last's)")
    ap.add_argument("--game", type=Path, required=True, help="the folder of mkw / mkw.exe")
    ap.add_argument("--romfs", choices=("copy", "link"), help="(default: the last setup's, else copy)")
    ap.add_argument("--jobs", type=int, default=0, help="parallel jobs (default: from cores and memory)")
    ap.add_argument("--cc", help="the C compiler command (default: the package's zig cc, else clang)")
    ap.add_argument("--glslc", help="(default: the package's, else glslc on PATH)")
    ap.add_argument("--target", choices=("linux", "windows"), default="windows" if os.name == "nt" else "linux")
    ap.add_argument("--port", default="", help="the port version this setup is for (the start screen's): kept in the state")
    args = ap.parse_args()
    own_children()
    if ca := update.ca_file():  # spec.py downloads NVIDIA's redistributables with this Python (issue #102, NixOS)
        os.environ["SSL_CERT_FILE"] = ca
    game = args.game.resolve()
    state = load_state(game)
    if not args.source:  # the last setup's; before 0.1.5 the state kept them only in its keys
        last = state.get("source") or (str(Path(state["done"]["exefs"]).parent) if "exefs" in state["done"] else "")
        if not last:
            emit("error", "source", "no earlier setup in this folder")
            sys.exit(2)
        args.source = Path(last)
        args.romfs = args.romfs or state.get("romfs_mode") or state["done"].get("romfs", "copy").split(" ", 1)[0]
    args.romfs = args.romfs or "copy"
    src = args.source.resolve()
    state["source"], state["romfs_mode"] = str(src), args.romfs
    # The start screen reads only stdout: the tools' output and the timings go to logs/setup.log, the file a report
    # attaches (issue #2 had none to give).
    (game / "logs").mkdir(parents=True, exist_ok=True)
    sys.stderr = open(game / "logs/setup.log", "w", encoding="utf-8", buffering=1)  # noqa: SIM115
    print(f"setup: {' '.join(sys.argv)}", file=sys.stderr)
    jobs = args.jobs or max(1, min(os.cpu_count() or 1, int(mem_gb() * 0.6)))
    glslc = args.glslc or default_glslc(game)
    work = game / "setup-work"
    try:
        emit("step", "check")
        cc = args.cc.split() if args.cc else default_cc(game, args.target)
        exefs_src, romfs_src, version = check(src)
        romfs = game / "romfs" if args.romfs == "copy" else romfs_src
        keys = {
            "exefs": str(exefs_src),
            "romfs": f"{args.romfs} {romfs_src}",
            "generate": f"{build.game_id()} {exefs_src} {version.name} {code_key(GENERATOR)}",
            "compile": f"{build.game_id()} {exefs_src} {version.name} {args.target} {code_key(COMPILER)}",
            "shaders": f"{exefs_src} {romfs_src} {translator_key()}",
            "warm": f"{build.game_id()} {romfs_src}",
        }
        for step in STEPS[1:]:
            # The start screen sets up again when its version differs (launcher.cpp): an update by hand (a new
            # AppImage or zip over the old) kept the old steps (#50: 0.1.8 played with 0.1.7's shaders). Kept once the
            # game is built, before the optional warm-up: a warm-up that keeps failing (#50's hang) must not repeat
            # the setup at every start.
            if step == "warm" and args.port:
                state["port"] = args.port
                save_state(game, state)
            key = keys.get(step, "")
            if step != "cleanup" and state["done"].get(step) == key:
                emit("skip", step)
                continue
            emit("step", step)
            if step == "exefs":
                copy_tree(exefs_src, game / "exefs", sum(1 for p in exefs_src.iterdir() if p.is_file()))
            elif step == "romfs":
                if args.romfs == "copy":
                    copy_tree(romfs_src, romfs, version.romfs_files)
                    ini_set(game / "mkw.ini", "romfs", "romfs")
                else:
                    ini_set(game / "mkw.ini", "romfs", str(romfs_src))
            elif step == "generate":
                generate(game / "exefs", work, jobs, version)
            elif step == "compile":
                compile_game(game, game / "exefs", work, cc, args.target, jobs)
            elif step == "shaders":
                shaders(game / "exefs", romfs, game, work, glslc, args.jobs or shader_jobs(), cc)
            elif step == "warm":
                warm(game, work)
            elif step == "cleanup":
                for p in work.glob("*"):
                    if p.is_dir() and p.name != "obj":
                        shutil.rmtree(p, ignore_errors=True)
                # the languages the game does not ship (ca, gl, eu), from this version's translations (the step runs
                # at every setup, so an update brings its new texts); about a second
                game_texts.build_all(game, romfs)
            state["done"][step] = key
            save_state(game, state)
        emit("done")
    except Failed as e:
        print(f"failed: {e}", file=sys.stderr)
        emit("error", e.code, e.detail)
        sys.exit(1)
    except OSError as e:
        emit("error", "io", e)
        sys.exit(1)


if __name__ == "__main__":
    main()
