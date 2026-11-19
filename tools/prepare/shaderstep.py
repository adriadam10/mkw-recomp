"""The shaders step of prepare.py: the SM86 spec built here from NVIDIA's nvdisasm, then the game's shaders extracted
from the RomFS and translated to SPIR-V; and the step's key.

Why apart: prepare.py holds the order of the steps and their keys; this step drives its own tools. Not run on its own.
"""

import hashlib
import json
import sys
from collections.abc import Callable
from pathlib import Path

from beside import Beside
from machine import Failed, crt_spec, run, tool_env

ROOT = Path(__file__).resolve().parents[2]  # the repository, or the package's setup/src
SHIPPED = ROOT / "FILES"  # the package's sources, one relative path per line (tools/build/dist.py)
sys.path.insert(0, str(ROOT / "tools/shaders"))
import pack  # noqa: E402  (spirv/shaders.pack)
import pipeline_pairs  # noqa: E402  (MAGIC, VERSION: the format the runtime of this package reads)

PAIRS_VERSION = pipeline_pairs.VERSION
PACK_VERSION = pack.VERSION


def shipped(files: list[Path]) -> list[Path]:
    """The files of a key that this package ships: a package unzipped by hand over an older one (#50's update) kept
    the files the new one moved or dropped, and they changed the keys (18/11/2026: 0.1.13's sm86/translate/flow.py,
    moved to frontend/, made the update redo the shaders). A source checkout has no list: every file counts. Paths are
    compared resolved: the AppImage's setup/ is a symlink into its mount and ROOT is resolved, so a file named through
    the symlink was not under it (#632: a setup run through the game folder's setup/ stopped at check)."""
    if not SHIPPED.is_file():
        return files
    names = set(SHIPPED.read_text(encoding="utf-8").splitlines())
    return [f for f in files if f.resolve().relative_to(ROOT).as_posix() in names]


def translator_key() -> str:
    """The shader step's key: every file the translation's output depends on (the translator, the decoder, extract.py
    and translate.py), as translate.py's translator_hash minus the spec, which this step builds. With translate.py
    alone, an update that changed only sm86/translate kept the old SPIR-V (#26: 0.1.7's translator fixes and the
    position default never reached a set-up game)."""
    h = hashlib.sha256()
    files = sorted((ROOT / "sm86/translate").rglob("*.py")) + sorted((ROOT / "sm86/decode").rglob("*.py"))
    for f in shipped([*files, ROOT / "tools/shaders/extract.py", ROOT / "tools/shaders/translate.py"]):
        h.update(f.read_bytes())
    return h.hexdigest()[:16]


def pairs_current(path: Path) -> bool:
    """Whether path is a pipeline-pairs.bin of this package's format."""
    try:
        with path.open("rb") as f:
            head = f.read(12)
    except OSError:
        return False
    return head[:8] == pipeline_pairs.MAGIC and int.from_bytes(head[8:12], "little") == PAIRS_VERSION


def ensure_pairs(exefs: Path, romfs: Path, work: Path, dest: Path) -> None:
    """pipeline-pairs.bin of this package's format in dest, for the warm step: an update whose shaders key did not
    change kept the older format, which the new runtime rejects, and prewarmed nothing (the compute programs joined it
    in version 3, 18/11/2026); and an update set up beside (--next) with the shaders skipped had none in setup-next/.
    A few seconds, with extract.py again when setup-work/shaders is gone."""
    out, sh = dest / "pipeline-pairs.bin", work / "shaders"
    if pairs_current(out):
        return
    if not (sh / "index.csv").is_file():
        run([sys.executable, str(ROOT / "tools/shaders/extract.py"), "--romfs", str(romfs), "--exefs", str(exefs),
             "--out", str(sh)])  # fmt: skip
    run([sys.executable, str(ROOT / "tools/shaders/pipeline_pairs.py"), "--shaders", str(sh), "--out", str(out)])


def ensure_pack(spirv: Path) -> None:
    """spirv/shaders.pack written again when the shaders step changed the folder after it, or none is there: the boot
    prewarm reads every program from it, 0.7 s instead of 24 s with the file cache cold on the Windows PC (prep-pack,
    18/11/2026). For the warm step, whose mkw --prewarm already reads it; a few seconds."""
    if spirv.is_dir() and not pack.current(spirv):
        n, size = pack.pack(spirv)
        print(f"warm: {n} programs packed, {size / 2**20:.0f} MiB", file=sys.stderr)


def spec_loads(spec: Path) -> bool:
    """Whether the decoder accepts this spec (0.3 s). Until 0.1.12 a capture the parser rejected still left its
    instructions.json behind, and every later setup failed on it in translate.py (issue #325)."""
    sys.path.insert(0, str(ROOT / "sm86/decode"))
    import sass_spec  # noqa: PLC0415 - sm86/decode is not a package

    try:
        sass_spec.Spec(str(spec))
    except (ValueError, KeyError, TypeError) as e:
        print(f"{spec} is broken ({type(e).__name__}: {str(e)[:200]}): building it again", flush=True)
        return False
    return True


def sm86_spec(game: Path, work: Path, cc: list[str]) -> Path:
    """The spec translate.py decodes with: a source checkout's own build/sm86, else built once into <game>/sm86."""
    for spec in (ROOT / "build/sm86/instructions.json", game / "sm86/instructions.json"):
        if spec.is_file() and spec_loads(spec):
            return spec
    crt = crt_spec(game, cc)  # spec.py links a DLL too on Windows: without it, zig builds its libc there (issue #2)
    run(spec_command(game, work, cc, crt))
    return game / "sm86/instructions.json"


def spec_command(game: Path, work: Path, cc: list[str], crt: Path | None) -> list[str]:
    """spec.py's command line; the compiler goes as JSON, whose paths may have spaces (issue #29)."""
    return [sys.executable, str(ROOT / "sm86/spec/spec.py"), "--out", str(game / "sm86"), "--cache",
            str(work / "nvidia"), "--cc", json.dumps(cc), *(["--crt", str(crt)] if crt else [])]  # fmt: skip


def tonemap_key(shaders_key: str) -> str:
    """The HDR tone mapper's programs' own key (prepare.py "tonemap"): the shaders step's, whose SPIR-V they are made
    from, and renodx.py's own hash. Apart from the shaders key so that a change to renodx.py redoes only renodx.py
    (seconds), not the translation of every program for every player."""
    h = hashlib.sha256()
    for f in shipped([ROOT / "tools/shaders/renodx.py"]):
        h.update(f.read_bytes())
    return f"{shaders_key} renodx {h.hexdigest()[:16]}"


def tonemap(game: Path, dest: Path, glslc: str) -> bool:
    """<dest>/spirv-renodx: RenoDX's tone mapping for the HDR mode (MKW_HDR_TONEMAP, gfx_shader.c), from the translated
    programs (dest's spirv/, or the played one's when an update set up beside it skipped the shaders), with the
    spirv-cross, glslangValidator and spirv-val the package carries beside glslc. Beside spirv/, never in it: a write in
    spirv/ would make its shaders.pack stale. Optional: a failure leaves the game's own programs, and the setup goes
    on; False then, so the next setup tries again."""
    spirv = dest / "spirv" if (dest / "spirv").is_dir() else game / "spirv"
    if not spirv.is_dir():
        return False
    try:
        run([sys.executable, str(ROOT / "tools/shaders/renodx.py"), "--spirv", str(spirv),
             "--out", str(dest / "spirv-renodx"), "--tools", str(Path(glslc).parent)], env=tool_env(glslc))  # fmt: skip
    except Failed as e:
        print(f"tonemap: renodx.py: {e} (the HDR tone mapper stays the game's)", file=sys.stderr)
        return False
    return True


def shaders(
    exefs: Path,
    romfs: Path,
    game: Path,
    work: Path,
    glslc: str,
    jobs: Callable[[], int],
    cc: list[str],
    dest: Path | None = None,
    before_translate: Callable[[], None] = lambda: None,
    on_progress: Callable[[str], None] | None = None,
) -> None:
    """spirv/ and pipeline-pairs.bin into dest (default the game folder; prepare.py --next: setup-next/).
    before_translate waits for what translate.py must not run beside (run beside generate, beside.py: generate's
    workers, and setup-next/spirv's links); jobs is asked then, from the memory free at that moment."""
    dest = dest or game
    dest.mkdir(parents=True, exist_ok=True)
    spec = sm86_spec(game, work, cc)
    sh = work / "shaders"
    run([sys.executable, str(ROOT / "tools/shaders/extract.py"), "--romfs", str(romfs), "--exefs", str(exefs),
         "--out", str(sh)])  # fmt: skip
    # every program pair of the copy, for the boot's shader library prewarm (runtime/gfx/vk/pipe/vk_pipe_pairs.c); it
    # reads only the extracted shaders, so it runs beside translate.py. Before it, beside generate on the Windows PC,
    # its 6.6 s took 124 s and held translate.py back (18/11/2026)
    pairs = Beside("pairs", lambda _: run([sys.executable, str(ROOT / "tools/shaders/pipeline_pairs.py"),
                                          "--shaders", str(sh), "--out", str(dest / "pipeline-pairs.bin")]))  # fmt: skip
    before_translate()
    n = jobs()
    print(f"shaders: translate.py with {n} workers", file=sys.stderr)
    # --next: the played version's outputs linked program by program in translate.py's workers; nextsetup.seed linked
    # all 46 297 files first, one after another (46 s on the Windows PC, 18/11/2026)
    base = ["--base", str(game / "spirv")] if dest != game and (game / "spirv").is_dir() else []
    while True:
        try:
            run([sys.executable, str(ROOT / "tools/shaders/translate.py"), "--shaders", str(sh),
                 "--out", str(dest / "spirv"), "--glslc", glslc, "--no-validate", "--progress", "--jobs", str(n),
                 "--spec", str(spec), *base], env=tool_env(glslc), progress_prefix="progress",
                on_progress=on_progress)  # fmt: skip
            break
        except Failed as e:
            # A worker that dies (the system ends it when memory runs out) breaks the whole pool (issue #24,
            # BrokenProcessPool). What is translated stays (translate.py resumes); half the workers need half the memory.
            if "BrokenProcessPool" not in str(e) or n == 1:
                raise
            n = max(1, n // 2)
            print(f"shaders: a worker ended abruptly, again with {n} workers", file=sys.stderr)
    for p in (dest / "spirv").glob("*.glsl"):  # the runtime reads .spv and .res.json; the GLSL only made them
        p.unlink()
    pairs.join()
