"""Translate every extracted program to SPIR-V offline and report what fails: the shader translator's work list.

Why: shaders are translated before the game runs, never in play. Each program goes SM86 -> GLSL (sm86/translate) ->
SPIR-V (glslc) and is checked by spirv-val; the census groups failures by the instruction class or stage that is not
handled yet, ranked by how many programs each blocks.

Usage: uv run tools/shaders/translate.py [--shaders build/shaders] [--out build/spirv] [--stage ps] [--top N]
                                         [--dumped DIR] [--spec FILE] [--glslc PATH] [--no-validate] [--progress]
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
import subprocess
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "sm86/translate"), str(ROOT / "sm86/translate/frontend")]
sys.path.insert(0, str(ROOT / "tools/shaders"))

import glsl
from extract import sph_stage, trim
from program import Decoder
from shaderc_lib import Shaderc

GLSLC_STAGE = {"ps": "frag", "vs": "vert", "cs": "comp", "task": "task", "mesh": "mesh"}
_dec = None
_shaderc: Shaderc | None = None  # libshaderc in this process when found; else one glslc a program


SPEC = ROOT / "build/sm86/instructions.json"
GLSLC = "glslc"
VALIDATE = True


def init(spec: Path = SPEC, glslc: str = GLSLC, validate: bool = True) -> None:
    """Each worker: the decoder, and the compiler settings of --glslc and --no-validate (workers do not see main's)."""
    global _dec, GLSLC, VALIDATE, _shaderc
    _dec = Decoder(spec)
    GLSLC, VALIDATE = glslc, validate
    _shaderc = Shaderc.near(glslc)


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


def work(job: tuple[str, str, str, str, bool, str]) -> tuple[str, str]:
    """(sha1, 'ok' or the reason it failed)."""
    sha, stage, shaders, out, full, th = job
    code = trim(Path(shaders, f"{sha}.code").read_bytes(), stage)  # a no-op on extract.py's output; cuts dumps
    # File name = the key the runtime computes (runtime/gfx/nvn/procs/nvn2.c nvn2_program_key): sha256 of the 0xd8-byte
    # control section and the code as extract.py cut it.
    ctl = Path(shaders, f"{sha}.ctl").read_bytes()
    key = hashlib.sha256(ctl + code).hexdigest()
    done = Path(out, f"{key}.done")
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
    done.write_text(f"{th}\n{why}\n{glsl_sum}")
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
    cs_src = refl.pop("cs_glsl", None)
    gl, spv, res = Path(out, f"{key}.glsl"), Path(out, f"{key}.spv"), Path(out, f"{key}.result")
    if cs_src is not None:   # task/mesh as compute, for devices without mesh shaders
        cs_gl, cs_spv = Path(out, f"{key}.cs.glsl"), Path(out, f"{key}.cs.spv")
        if full or not cs_spv.exists() or not cs_gl.exists() or cs_gl.read_text() != cs_src:
            cs_gl.write_text(cs_src)
            cs_why = compile_check(cs_gl, cs_spv, "cs")
            if cs_why != "ok":
                cs_spv.unlink(missing_ok=True)
                Path(out, f"{key}.cs.result").write_text(cs_why)
    # The reflection first: a change that touches only it (the written flag of storage buffers, 01/11/2026) leaves the
    # GLSL as it was, and the shortcut below kept the old file.
    Path(out, f"{key}.res.json").write_text(json.dumps(refl, indent=1))
    glsl_sum = hashlib.sha256(src.encode()).hexdigest()
    same = prev_sum == glsl_sum or (gl.exists() and gl.read_text() == src)
    if not full and res.exists() and spv.exists() and same:
        return res.read_text(), glsl_sum  # same GLSL as the last pass: same SPIR-V and verdict
    res.unlink(missing_ok=True)  # until this pass has a verdict
    gl.write_text(src)
    why = compile_check(gl, spv, refl["stage"])
    res.write_text(why)
    return why, glsl_sum


def compile_check(gl: Path, spv: Path, stage: str) -> str:
    """'ok' or why glslc or spirv-val refused the GLSL."""
    if _shaderc:
        code, err = _shaderc.compile(gl.read_text(), stage, str(gl))
        if code:
            spv.write_bytes(code)
    else:
        r = subprocess.run([GLSLC, "--target-env=vulkan1.3", f"-fshader-stage={GLSLC_STAGE[stage]}", "-O", str(gl),
                            "-o", str(spv)], capture_output=True, text=True, check=False)  # fmt: skip
        err = r.stderr if r.returncode else ""
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
    gl.write_text(MESH_CS_VS)
    refl = {"stage": "vs", "cbufs": [], "sbufs": [], "textures": [], "images": [], "spec": []}
    Path(out, f"{MESH_CS_VS_KEY}.res.json").write_text(json.dumps(refl, indent=1))
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
    init_args = (args.spec, args.glslc, not args.no_validate)
    with ProcessPoolExecutor(args.jobs, initializer=init, initargs=init_args) as pool:
        for n, (sha, why) in enumerate(pool.map(work, jobs), 1):
            if args.progress and (n % 100 == 0 or n == len(jobs)):
                print(f"progress {n} {len(jobs)}", flush=True)
            results[why] += 1
            if why != "ok":
                failed.setdefault(why, []).append(sha)
    # The work list per reason, for the next translator change: <out>/failed.tsv (reason, programs).
    with (args.out / "failed.tsv").open("w") as f:
        for why, shas in sorted(failed.items(), key=lambda x: -len(x[1])):
            f.write(f"{why}\t{' '.join(shas)}\n")
    ok = results.pop("ok", 0)
    print(f"{len(jobs)} programs: {ok} valid SPIR-V ({ok / max(1, len(jobs)):.1%})")
    for why, n in results.most_common(args.top):
        print(f"{n:>6}  {why}")


if __name__ == "__main__":
    main()
