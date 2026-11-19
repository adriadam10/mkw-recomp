"""Translate every extracted program to SPIR-V offline and report what fails: the shader translator's work list.

Why: shaders are translated before the game runs, never in play. Each program goes SM86 -> GLSL (sm86/translate) ->
SPIR-V (glslc) and is checked by spirv-val; the census groups failures by the instruction class or stage that is not
handled yet, ranked by how many programs each blocks.

Usage: uv run tools/shaders/translate.py [--shaders build/shaders] [--out build/spirv] [--stage ps] [--top N]
                                         [--dumped DIR] [--spec FILE] [--glslc PATH] [--no-validate] [--progress]
                                         [--base DIR]
Writes <out>/<key>.{glsl,spv,res.json,result} and <out>/failed.tsv (each failure reason with its programs). A program whose GLSL comes out the same as in the last pass keeps its
SPIR-V and result without glslc or spirv-val (glslc is ~75 % of the time: 196 ms a program, 30/10/2026), so after a
translator change only the programs it touched are compiled again; --full compiles everything. The GLSL's sha256 also
goes in <key>.done, so the shortcut holds where the GLSL was deleted after the pass (a player's setup keeps only .spv and
.res.json: before, every translator change in an update compiled all ~11 500 programs again). --dumped also takes the programs a run could not find
(MKW_SPIRV_DUMP=DIR): ones the game builds in play from templates in main, which no extraction can list.
"""

import argparse
import csv
import hashlib
import json
import os
import shutil
import subprocess
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
sys.path.insert(0, str(ROOT / "tools/shaders"))

import glsl
from glsl import int_hints
from extract import sph_stage, trim
from program import Decoder
import shaderc_lib
from shaderc_lib import Shaderc

GLSLC_STAGE = {"ps": "frag", "vs": "vert", "cs": "comp", "task": "task", "mesh": "mesh"}
# glslc without optimization: -O was 86 % of glslc's time (366 -> 140 s over 11 561 programs on 12 threads) for SPIR-V
# 2.5 times larger; the driver optimizes it again (prewarm +27 s on RADV) and the frame's GPU time stayed the same
# (19.5 -> 19.2 ms replaying cap/race on a GTX 1050 Ti, 18/11/2026). Here, not in shaderc_lib, so the step's key
# (translator_key) changes and a set-up game translates again.
GLSLC_O = 0
_dec = None
_shaderc: Shaderc | None = None  # libshaderc in this process when found; else one glslc a program


SPEC = ROOT / "build/sm86/instructions.json"
GLSLC = "glslc"
VALIDATE = True
BASE = ""  # --base
# a program's outputs (<key><suffix>), linked from --base before its pass
OUTPUTS = (".done", ".spv", ".result", ".res.json", ".cs.spv", ".cs.sum", ".cs.result")


def init(spec: Path = SPEC, glslc: str = GLSLC, validate: bool = True, base: str = "") -> None:
    """Each worker: the decoder, and the settings of --glslc, --no-validate and --base (workers do not see main's)."""
    global _dec, GLSLC, VALIDATE, _shaderc, BASE
    _dec = Decoder(spec)
    GLSLC, VALIDATE, BASE = glslc, validate, base
    shaderc_lib.PERFORMANCE = GLSLC_O
    _shaderc = Shaderc.near(glslc)


def link_base(key: str, out: str) -> None:
    """The program's outputs of the --base pass hard-linked into out (a copy where the file system has no links): the
    pass then reuses what did not change, as in its own folder. put() renames over a link, never writes through it."""
    for suffix in OUTPUTS:
        src, dst = Path(BASE, key + suffix), Path(out, key + suffix)
        try:
            os.link(src, dst)
        except FileNotFoundError, FileExistsError:
            pass
        except OSError:
            shutil.copy2(src, dst)


def translator_hash(spec: Path = SPEC) -> str:
    """What a program's result depends on besides the program: the translator, the decoder and its specification.
    A pass with the same hash and an unchanged program reuses <key>.done without decoding or translating (decoding
    was 33 ms a program, ~1 min a pass over 11 561 programs, 31/10/2026)."""
    h = hashlib.sha256()
    files = sorted((ROOT / "sm86/translate").rglob("*.py")) + sorted((ROOT / "sm86/decode").rglob("*.py"))
    for f in [*files, ROOT / "tools/shaders/extract.py", ROOT / "tools/shaders/translate.py"]:
        h.update(f.read_bytes())
    h.update(f"{spec.stat().st_size} {spec.stat().st_mtime_ns}".encode())
    h.update(str(os.environ.get("SM86_SPEC", "0") == "1").encode())  # glsl/core.py SPEC_ON
    return h.hexdigest()


def put(path: Path, data: str | bytes) -> None:
    """The file written beside and renamed over: a setup stopped halfway leaves no half-written output, and an output
    folder seeded with hard links to an older one (an update prepared beside the version still played) never writes
    through to the older file."""
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data) if isinstance(data, bytes) else tmp.write_text(data)
    os.replace(tmp, path)


def work(job: tuple[str, str, str, str, bool, str]) -> tuple[str, str]:
    """(sha1, 'ok' or the reason it failed)."""
    sha, stage, shaders, out, full, th = job
    code = trim(Path(shaders, f"{sha}.code").read_bytes(), stage)  # a no-op on extract.py's output; cuts dumps
    # File name = the key the runtime computes (runtime/gfx/nvn/procs/nvn2.c nvn2_program_key): sha256 of the 0xd8-byte
    # control section and the code as extract.py cut it.
    ctl = Path(shaders, f"{sha}.ctl").read_bytes()
    key = hashlib.sha256(ctl + code).hexdigest()
    done = Path(out, f"{key}.done")
    if BASE and not done.exists():
        link_base(key, out)
    prev_sum = ""
    if not full and done.exists():
        # "<translator hash>\n<verdict>[\n<GLSL sha256>]": verdicts are one line; files of older passes have no sum
        prev_th, why, prev_sum = (done.read_text().split("\n") + [""])[:3]
        if prev_th == th and (why != "ok" or Path(out, f"{key}.spv").exists()):
            return sha, why
    c1 = Path(shaders, f"{sha}.c1")
    why, glsl_sum = translate_one(
        sha, stage, code, key, out, full, ctl, c1.read_bytes() if c1.exists() else b"", prev_sum
    )
    put(done, f"{th}\n{why}\n{glsl_sum}")
    return sha, why


def translate_one(
    sha: str, stage: str, code: bytes, key: str, out: str, full: bool, ctl: bytes, c1: bytes = b"", prev_sum: str = ""
) -> tuple[str, str]:
    """(verdict, sha256 of the GLSL or "" when there is none)."""
    sph, insns = _dec.program(code, stage)
    try:
        src, refl = glsl.translate(sph, insns, stage, glsl.compute_shape(ctl) if stage == "cs" else None, c1)
    except glsl.Unsupported as e:
        return f"unsupported: {e}", ""
    except (KeyError, ValueError, IndexError) as e:  # a translator bug in one program must not stop the census
        return f"error: {type(e).__name__} {e}", ""
    refl.update(int_hints.hints(src, stage, refl))  # integer inputs and outputs for the setup's prewarm
    if stage == "cs":
        # The NVN2 c0 table's counts (runtime gfx_shader_refl.c cs_res), which the runtime otherwise reads from the
        # control section the game passes: the prewarm loads programs by key alone, and without them a compute
        # pipeline read its buffers from the wrong slots (0cb78db5 looped 65 536 times, 412 ms a frame, 18/11/2026).
        refl["cs_table"] = ",".join(str(ctl[o]) for o in (0xB1, 0xB2, 0xB3, 0xB6, 0xA9))
    cs_src = refl.pop("cs_glsl", None)
    gl, spv, res = Path(out, f"{key}.glsl"), Path(out, f"{key}.spv"), Path(out, f"{key}.result")
    if cs_src is not None:   # task/mesh as compute, for devices without mesh shaders
        cs_gl, cs_spv = Path(out, f"{key}.cs.glsl"), Path(out, f"{key}.cs.spv")
        # its GLSL's sum, not the GLSL: the setup deletes every .glsl after the pass, so a comparison with the file
        # compiled these again at every update
        cs_sum, cs_sum_file = hashlib.sha256(cs_src.encode()).hexdigest(), Path(out, f"{key}.cs.sum")
        if full or not cs_spv.exists() or not cs_sum_file.exists() or cs_sum_file.read_text() != cs_sum:
            put(cs_gl, cs_src)
            cs_why = compile_check(cs_gl, cs_spv, "cs")
            if cs_why != "ok":
                cs_spv.unlink(missing_ok=True)
                put(Path(out, f"{key}.cs.result"), cs_why)
            put(cs_sum_file, cs_sum)
    # The reflection first: a change that touches only it (the written flag of storage buffers, 01/11/2026) leaves the
    # GLSL as it was, and the shortcut below kept the old file.
    put(Path(out, f"{key}.res.json"), json.dumps(refl, indent=1))
    glsl_sum = hashlib.sha256(src.encode()).hexdigest()
    same = prev_sum == glsl_sum or (gl.exists() and gl.read_text() == src)
    if not full and res.exists() and spv.exists() and same:
        return res.read_text(), glsl_sum  # same GLSL as the last pass: same SPIR-V and verdict
    res.unlink(missing_ok=True)  # until this pass has a verdict
    put(gl, src)
    why = compile_check(gl, spv, refl["stage"])
    put(res, why)
    return why, glsl_sum


def compile_check(gl: Path, spv: Path, stage: str) -> str:
    """'ok' or why glslc or spirv-val refused the GLSL."""
    if _shaderc:
        code, err = _shaderc.compile(gl.read_text(), stage, str(gl))
        if code:
            put(spv, code)
    else:
        tmp = spv.with_name(spv.name + ".tmp")
        r = subprocess.run([GLSLC, "--target-env=vulkan1.3", f"-fshader-stage={GLSLC_STAGE[stage]}", f"-O{GLSLC_O}", str(gl),
                            "-o", str(tmp)], capture_output=True, text=True, check=False)  # fmt: skip
        err = r.stderr if r.returncode else ""
        if not err:
            os.replace(tmp, spv)
        tmp.unlink(missing_ok=True)
    if err:
        first = next((ln for ln in err.splitlines() if "error" in ln), err.strip()[:120])
        return f"glslc: {first.split(': ', 2)[-1][:100]}"
    if not VALIDATE:
        return "ok"
    v = subprocess.run(
        ["spirv-val", "--target-env", "vulkan1.3", str(spv)], capture_output=True, text=True, check=False
    )
    return "ok" if not v.returncode else f"spirv-val: {v.stderr.strip()[:100]}"


def write_mesh_cs_vs(out: Path) -> None:
    """The vertex shader of task/mesh as compute (glsl.head.MESH_CS_VS), as a prepared shader under its fixed key."""
    from glsl.head import MESH_CS_VS, MESH_CS_VS_KEY

    gl, spv = Path(out, f"{MESH_CS_VS_KEY}.glsl"), Path(out, f"{MESH_CS_VS_KEY}.spv")
    if spv.exists() and gl.exists() and gl.read_text() == MESH_CS_VS:
        return
    put(gl, MESH_CS_VS)
    refl = {"stage": "vs", "cbufs": [], "sbufs": [], "textures": [], "images": [], "spec": []}
    put(Path(out, f"{MESH_CS_VS_KEY}.res.json"), json.dumps(refl, indent=1))
    why = compile_check(gl, spv, "vs")
    if why != "ok":
        raise SystemExit(f"mesh-as-compute vertex shader: {why}")


def main() -> None:
    global GLSLC, VALIDATE
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shaders", type=Path, default=ROOT / "build/shaders")
    ap.add_argument("--out", type=Path, default=ROOT / "build/spirv")
    ap.add_argument("--stage", help="only this stage (ps, vs, cs...)")
    ap.add_argument("--task-mesh", action="store_true",
                    help="only the task and mesh programs (control+0x56: 6, 7), which the index lists as vs or tes?")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument(
        "--sample",
        type=int,
        default=0,
        help="only N programs spread over the list: a quick check of a change that touches every program",
    )
    ap.add_argument("--dumped-only", action="store_true", help="with --dumped: only those programs")
    ap.add_argument("--full", action="store_true", help="compile every program, also those whose GLSL is unchanged")
    ap.add_argument("--dumped", type=Path, help="also the <key>.{ctl,code} a run dumped (MKW_SPIRV_DUMP)")
    ap.add_argument("--spec", type=Path, default=SPEC, help="the SM86 spec (sm86/spec/spec.py)")
    ap.add_argument("--glslc", default=GLSLC, help="the glslc to run (the player's setup ships its own)")
    ap.add_argument("--no-validate", action="store_true", help="no spirv-val: the player's setup ships none")
    ap.add_argument("--progress", action="store_true", help="a line 'progress <done> <total>' every 100 programs")
    ap.add_argument(
        "--base",
        type=Path,
        help="an earlier pass's folder (the played version's spirv/, an update beside it): each program's outputs are "
        "linked from it and reused when unchanged, so --out holds only this pass's programs",
    )
    # Half the logical processors: the largest programs go first and glslc -O takes up to 0.75 GB on one of them with
    # the specialization constants (5.4 s); one per processor ended in the job's OOM at 6.7 GB (01/11/2026).
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2), help="parallel translations")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.shaders / "index.csv").open() as f:
        rows = [r for r in csv.DictReader(f) if not args.stage or r["stage"] == args.stage]
    if args.task_mesh:
        rows = [r for r in rows if (args.shaders / f"{r['sha1']}.ctl").read_bytes()[0x56] in (6, 7)]
    rows.sort(key=lambda r: -int(r["code_bytes"]))  # largest first: no long tail on one core at the end
    if args.sample and args.sample < len(rows):
        rows = rows[:: len(rows) // args.sample][: args.sample]
    th = translator_hash(args.spec)
    jobs = (
        []
        if args.dumped_only
        else [(r["sha1"], r["stage"], str(args.shaders), str(args.out), args.full, th) for r in rows]
    )
    for code in sorted(args.dumped.glob("*.code")) if args.dumped else ():
        stage = sph_stage(code.read_bytes()) or "cs"
        if not args.stage or stage == args.stage:
            jobs.append((code.stem, stage, str(args.dumped), str(args.out), args.full, th))
    # main compiles this one itself: it needs --glslc and --no-validate too, or the player's setup, which has neither
    # glslc nor spirv-val on PATH, fails here (Windows, v0.1.2 before release, 09/11/2026)
    init(args.spec, args.glslc, not args.no_validate)
    write_mesh_cs_vs(args.out)
    results = Counter()
    failed: dict[str, list[str]] = {}
    init_args = (args.spec, args.glslc, not args.no_validate, str(args.base or ""))
    with ProcessPoolExecutor(args.jobs, initializer=init, initargs=init_args) as pool:
        # blocks of 16 neighbours (by size: often variants of one program): a worker's word cache decodes 13 % fewer
        # words than with one program at a time (18/11/2026)
        for n, (sha, why) in enumerate(pool.map(work, jobs, chunksize=16), 1):
            if args.progress and (n % 100 == 0 or n == len(jobs)):
                print(f"progress {n} {len(jobs)}", flush=True)
            results[why] += 1
            if why != "ok":
                failed.setdefault(why, []).append(sha)
    # The work list per reason, for the next translator change: <out>/failed.tsv (reason, programs).
    # put: an output folder seeded with hard links to the played version's (prepare.py --next) must not write through
    rows = sorted(failed.items(), key=lambda x: -len(x[1]))
    put(args.out / "failed.tsv", "".join(f"{why}\t{' '.join(shas)}\n" for why, shas in rows))
    ok = results.pop("ok", 0)
    print(f"{len(jobs)} programs: {ok} valid SPIR-V ({ok / max(1, len(jobs)):.1%})")
    for why, n in results.most_common(args.top):
        print(f"{n:>6}  {why}")


if __name__ == "__main__":
    main()
