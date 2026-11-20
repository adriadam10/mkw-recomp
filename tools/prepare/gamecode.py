"""The compile step of prepare.py: the generated C into mkwgame.so / mkwgame.dll with the package's zig (or clang), its
chunks on LLVM's own clang when the package has it (fastcc.py).

Why apart: prepare.py holds the order of the steps and their keys; this step is the largest and has its own reuse of
earlier objects. Not run on its own.
"""

import hashlib
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path, PurePath

ROOT = Path(__file__).resolve().parents[2]  # the repository, or the package's setup/src
sys.path.insert(0, str(ROOT / "tools/build"))
import build  # (GEN_FLAGS, gen_modules, module_glue, game_id: the same game code as build.py --dist)

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fastcc
from machine import Failed, crt_link, emit, package_dir

# The compiler's messages as text: zig writes UTF-8 (paths with "Área de Trabalho" in it), which the locale's code page
# could not decode, and the reader thread's error left stdout None (#527: setup failed at compile on a Portuguese Windows)
UTF8 = {"encoding": "utf-8", "errors": "replace"}
LINK_TIMEOUT = 30 * 60  # far past a slow link: only a hung linker (issue #2: "stuck" after the last object)


class DamagedSource(Failed):
    """A generated C file with NUL bytes in it (#434: `uint6<NUL>_t` in main_0203.c, on Windows): the write that made it
    did not reach the disk whole (a crash or power cut after the size grew). Generate's key still matches, so every retry
    reused it and failed the same; prepare.py regenerates once on this."""


def cache_failure() -> str:
    cache = os.environ.get("ZIG_GLOBAL_CACHE_DIR")
    folder = f" ({cache})" if cache else ""
    return (f"the compiler could not use its cache folder{folder}, as on a network share or a drive a "
            "compatibility layer maps. Move the cache to a local drive, or set ZIG_GLOBAL_CACHE_DIR to a folder on one.")


def damaged(src: Path) -> bool:
    return b"\0" in src.read_bytes()


def write_link_response(rsp: Path, objects: list[Path], windows_quoting: bool) -> None:
    """Objects passed to the linker without the process command line's length limit."""
    # as_posix converts native Windows separators only: a backslash in a POSIX folder name is a literal character.
    # lld-link and the package's zig cc (0.17.1, also on Linux) preserve ordinary backslashes inside quotes;
    # the POSIX clang driver consumes backslash escapes there. Real linker regressions cover both grammars.
    paths = [o.as_posix() for o in objects]
    if windows_quoting:
        lines = [subprocess.list2cmdline([p]) for p in paths]
    else:
        lines = ['"' + p.replace("\\", "\\\\").replace('"', '\\"') + '"' for p in paths]
    rsp.write_text("\n".join(lines) + "\n", encoding="utf-8")


def cpu_level() -> str:
    """The x86-64 level the game code is built for: v3 (AVX2, FMA, BMI2: build.py ARCH_FLAGS) or v2 (SSE4.2), for CPUs
    without AVX2 (an AMD FX, an Intel Core before Haswell): MKW_SETUP_CPU=v2, or the package's own `setup/cpu-level`
    file (the -noavx2 package, release.py), which the environment overrides. Each level has its own object keys (the
    flags are in them)."""
    marker = Path(__file__).resolve().parents[3] / "cpu-level"
    packaged = marker.read_text().strip() if marker.is_file() else "v3"
    level = os.environ.get("MKW_SETUP_CPU", packaged)
    if level not in ("v2", "v3"):
        sys.exit(f"MKW_SETUP_CPU={level}: v2 or v3")
    return level


def compile_game(
    game: Path, exefs: Path, work: Path, cc: list[str], target: str, jobs: int, dest: Path | None = None
) -> None:
    """The game code library and its id into dest (default the game folder; prepare.py --next: setup-next/)."""
    dest = dest or game
    gen, obj = work / "generated", work / "obj"
    windows = target == "windows"
    flags = [*build.GEN_FLAGS, *([] if windows else ["-fPIC"])]
    v2 = cpu_level() == "v2"
    if v2:
        flags = [f.replace("-march=x86-64-v3", "-march=x86-64-v2") for f in flags]
    if Path(cc[0]).stem == "zig":
        # zig cc turns -O1 into -O2 with both vectorizers, keeps frame pointers, adds debug info and UBSan, and spells
        # the CPU its own way: 182 126 instructions on a chunk clang -O1 makes in 150 969. With these, 150 745 (the
        # rest is LLVM 22 against 23), as SavageWindRecomp's GAMELIB_ZIGFLAGS (runtime/gamebuild.c, MIT).
        # AArch64: zig ignores -ffixed-x18 without a word (mkwgame.so used x18 73 079 times and crashed at the first
        # preemption, Odin 2, 10/11/2026); its CPU feature reserve_x18 keeps the register out.
        flags = [f.replace("-march=x86-64-v3", "-mcpu=x86_64_v3").replace("-march=x86-64-v2", "-mcpu=x86_64_v2")
                 .replace("-march=armv8.2-a+fp16+rcpc+dotprod+crc",
                          "-mcpu=generic+v8_2a+fullfp16+rcpc+dotprod+crc+reserve_x18")
                 for f in flags if f != "-ffixed-x18"]  # fmt: skip
        flags += ["-Xclang", "-O1", "-fno-vectorize", "-fno-slp-vectorize", "-fomit-frame-pointer", "-g0",
                  "-fno-sanitize=all"]  # fmt: skip
    srcs = sorted(p for mod in build.gen_modules(gen) for p in (gen / mod).glob("*.c"))
    obj.mkdir(parents=True, exist_ok=True)
    (obj / "modules.c").write_text(build.module_glue(gen, exefs, True))
    gid = f'-DRT_GAME_ID="{build.game_id()}"'
    # every folder with a runtime header the package carries (runtime/cpu, runtime/core/boot for gamelib.h), as build.py
    # does: a fixed list missed gamelib.h when it moved to core/boot (issue #1, v0.1.1)
    inc = [f"-I{d}" for d in sorted({p.parent for p in (ROOT / "runtime").rglob("*.h")})] + [f"-I{gen}"]
    cpu = ROOT / "runtime/cpu"
    jobs_c = [(src, obj / f"{src.parent.name}_{src.stem}.o", [*flags, f"-I{cpu}"]) for src in srcs]
    if v2:  # the FMADDs and the RECPS/RSQRTS steps become fma/fmaf calls: correctly rounded ones, not the C runtime's
        jobs_c.append((cpu / "fma.c", obj / "cpu_fma.o", [*flags, f"-I{cpu}"]))
    jobs_c.append((obj / "modules.c", obj / "modules.o", [*flags, gid, *inc]))
    cc1 = fastcc.setup(cc, package_dir(game), obj, [jobs_c[0][2], jobs_c[-1][2]])
    jobs_c.sort(key=lambda j: -j[0].stat().st_size)  # the largest first: no 10 MB main_table.c alone at the end
    total, done = len(jobs_c) + 1, 0  # the link is the last unit: a progress bar full before it hid it (issue #2)
    extra = [str(package_dir(game) / "lib/libmkw.dll.a")] if windows else ["-Wl,-z,noexecstack"]
    # The chunks see runtime/cpu and only the generated headers they name: a new patch set changes the generated
    # patches.h, which no chunk includes, and recompiled all 359 of them (PR 450 on 16/11/2026, 9 min on the 6-thread
    # Windows PC). modules.c sees every folder and keeps the whole list.
    base_all = common_key(cc, sorted((ROOT / "runtime").rglob("*.h")) + sorted(gen.rglob("*.h")))
    base_cpu = common_key(cc, sorted(cpu.rglob("*.h")))
    gen_headers = sorted(gen.rglob("*.h"))
    compiled: list[Path] = []
    legacy = legacy_compilers(cc, game)
    old_all = [common_key(cc, sorted((ROOT / "runtime").rglob("*.h")) + gen_headers, compiler=c) for c in legacy]
    old_cpu = [common_key(cc, sorted(cpu.rglob("*.h")), compiler=c) for c in legacy]

    def one(job: tuple[Path, Path, list[str]]) -> str:
        src, out, f = job
        key = object_key(base_all if src.parent == obj else chunk_base(cc, base_cpu, gen_headers, src), src, f, gen)
        stamp = out.with_suffix(".key")
        if out.is_file() and stamp.is_file() and (had := stamp.read_text()) == key:
            return ""
        if (
            out.is_file()
            and stamp.is_file()
            and any(
                had == object_key(a if src.parent == obj else chunk_base(cc, c, gen_headers, src), src, f, gen)
                for a, c in zip(old_all, old_cpu, strict=True)
            )
        ):
            stamp.write_text(key)  # an object of the same compiler under its key before 0.1.14
            return ""
        compiled.append(src)  # list.append is atomic across the pool's threads
        stamp.unlink(missing_ok=True)
        return compile_one(cc, f, src, out, stamp, key, cc1.cmd(f, src, out) if cc1 else None)

    lib = dest / ("mkwgame.dll" if windows else "mkwgame.so")
    part = lib.with_name(lib.name + ".part")
    rsp = obj / "link.rsp"  # ~360 objects: past the Windows command line limit
    link = crt_link(game, cc, lib, part, obj, [f"@{rsp}", *extra]) if windows else None
    with ThreadPoolExecutor(jobs) as pool:
        # first, so it runs beside the compiles; not needed with the package's prebuilt C runtime (crt_link)
        probe = None if link else pool.submit(probe_link, cc, obj, windows)
        for err in pool.map(one, jobs_c):
            if err:
                raise Failed(f"compiling the game code failed: {err}")
            done += 1
            emit("progress", done, total)
        if probe:
            probe.result()
    # what an update costs here: how many chunks a release's change of the generated C really touches
    print(f"compile: {len(compiled)} of {len(jobs_c)} units compiled, the rest reused", file=sys.stderr)
    objs = [o for _, o, _ in jobs_c]
    for stale in set(obj.glob("*.o")) - {o for _, o, _ in jobs_c}:  # chunks an earlier version had
        stale.unlink()
        stale.with_suffix(".key").unlink(missing_ok=True)
    write_link_response(rsp, objs, windows or Path(cc[0]).stem == "zig")
    t0 = time.monotonic()
    try:
        r = subprocess.run(link or [*cc, "-shared", "-o", str(part), f"@{rsp}", *extra], capture_output=True,
                           **UTF8, check=False, timeout=LINK_TIMEOUT)  # fmt: skip
    except subprocess.TimeoutExpired as e:
        raise Failed(f"linking the game code took over {LINK_TIMEOUT // 60} min: {tail_of(e.stderr)}") from None
    print(f"link: {time.monotonic() - t0:.1f} s, rc {r.returncode}\n{r.stderr}", file=sys.stderr)
    if r.returncode:
        if "checking cache failed" in r.stderr:
            raise Failed(f"linking the game code failed: {cache_failure()}")
        raise Failed(f"linking the game code failed: {r.stderr.strip()[-300:]}")
    part.replace(lib)
    emit("progress", total, total)
    (dest / "mkwgame.id").write_text(build.game_id())  # runtime/core/boot/gamelib.c rt_game_present


def compile_one(
    cc: list[str], flags: list[str], src: Path, out: Path, stamp: Path, key: str, cmd: list[str] | None = None
) -> str:
    """One object: "" when built, else the error for the player; a damaged source raises DamagedSource instead.
    cmd: the command instead of cc's (fastcc.py's -cc1 line)."""
    cmd = cmd or [*cc, "-std=gnu11", *flags, "-c", str(src), "-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, **UTF8, check=False)
    if r.returncode and crashed(r.stderr):
        # A compiler crash (ICE) on a chunk that compiles everywhere else is the machine, not the source (#506: a
        # different chunk each time, only with parallel jobs, on an i7-13700KF): one more try, and its first lines
        # (the crash's signature, which the tail cut) kept in setup.log.
        print(f"compile: {src.name}: the compiler crashed, trying again:\n{r.stderr.strip()[:1500]}", file=sys.stderr)
        r = subprocess.run(cmd, capture_output=True, **UTF8, check=False)
    if r.returncode:
        if damaged(src):
            raise DamagedSource(f"compiling the game code failed: {src.name} has NUL bytes (a damaged write)")
        print(f"compile: {src.name} failed:\n{r.stderr.strip()[:3000]}", file=sys.stderr)
        if "checking cache failed" in r.stderr:  # #406: zig cannot use its cache folder
            return f"{src.name}: {cache_failure()}"
        return f"{src.name}: {r.stderr.strip()[-300:]}"
    stamp.write_text(key)
    return ""


def crashed(stderr: str) -> bool:
    """clang's crash report, as opposed to an error in the source."""
    return "PLEASE submit a bug report" in stderr or "FILES TO THE BUG REPORT" in stderr


def chunk_base(cc: list[str], base_cpu: str, gen_headers: list[Path], src: Path) -> str:
    """A chunk's common key: runtime/cpu's headers, plus the generated ones its C names."""
    text = src.read_bytes()
    named = [p for p in gen_headers if f'#include "{p.name}"'.encode() in text]
    return common_key(cc, named, base_cpu) if named else base_cpu


def compiler_id(cc: list[str]) -> str:
    """The compiler as far as its objects go: its version and its arguments, without the paths. `--version` also prints
    InstalledDir (the package's folder: the AppImage mounts it at a new /tmp/.mount_* each start) and, for zig, a
    Target with the host's kernel and glibc: in the key, every update on Linux compiled all ~360 chunks again
    (18/11/2026: the update from 0.1.13 to main, with 357 of its 358 chunks' C unchanged)."""
    out = subprocess.run([*cc, "--version"], capture_output=True, **UTF8, check=False).stdout
    keep = [ln for ln in out.splitlines() if not ln.startswith(("InstalledDir:", "Target:"))]
    return "\n".join([*keep, *cc[1:]])


def legacy_compilers(cc: list[str], game: Path) -> list[str]:
    """The compiler as releases up to 0.1.13 keyed it (its whole `--version`, InstalledDir and all), for each folder
    the played version's zig could have run from: the game folder's setup/ and this package's. A player's first
    update to the new key (dacd613) reuses the objects of the same compiler instead of compiling all ~360 chunks
    once more (9 min on the 6-thread Windows PC). Only Windows matches: the AppImage's mount there was a new
    /tmp/.mount_* each run. Drop it once 0.1.13 is no longer updated from."""
    out = subprocess.run([*cc, "--version"], capture_output=True, **UTF8, check=False).stdout
    if "InstalledDir:" not in out:
        return []
    exe = "zig.exe" if Path(cc[0]).name.endswith(".exe") else "zig"
    dirs = {s for d in (game / "setup/zig", package_dir(game) / "zig") if (d / exe).is_file() for s in installed_dirs(d)}
    return [re.sub(r"(?m)^InstalledDir: .*$", lambda _, d=d: f"InstalledDir: {d}", out) for d in sorted(dirs)]


def installed_dirs(d: PurePath) -> set[str]:
    """How `--version` may spell a folder in InstalledDir: zig on Windows writes C:/upd/x/setup/zig, with forward
    slashes (18/11/2026: the backslash form alone matched no 0.1.13 object, and the update compiled all ~360 again)."""
    return {str(d), d.as_posix()}


def common_key(cc: list[str], headers: list[Path], base: str = "", compiler: str = "") -> str:
    """What an object depends on besides its own C and flags: the compiler (or `base`, a key that already holds it)
    and every header it can include. `compiler`: that compiler's id instead of compiler_id(cc)."""
    h = hashlib.sha256(base.encode() if base else (compiler or compiler_id(cc)).encode())
    for p in headers:
        h.update(p.relative_to(p.parents[1]).as_posix().encode() + p.read_bytes())
    return h.hexdigest()


def object_key(base: str, src: Path, flags: list[str], gen: Path) -> str:
    """An object is reused while this matches: kept across setups (cleanup leaves obj/), an update that changes a few
    functions (a patch in patches-mkw.txt) compiles their chunks only, not all ~360 (8 min Linux, 12 Windows,
    11/11/2026). Paths leave the key: the AppImage mounts the package at a new /tmp/.mount_* each run."""
    h = hashlib.sha256(base.encode())
    for f in flags:
        h.update(f.replace(str(ROOT), "<src>").replace(str(gen), "<gen>").encode() + b"\0")
    h.update(src.read_bytes())
    return h.hexdigest()


def probe_link(cc: list[str], obj: Path, windows: bool) -> None:
    """A one-function library linked while the game code compiles: zig builds its libc and compiler_rt for the target
    on its first link (11.7 s for Linux, 22 s for Windows on 12 cores, 09/11/2026), silent after the last object, and
    caches them; this pays it in parallel. A failure is only logged: the real link reports its own."""
    src, lib = obj / "probe.c", obj / ("probe.dll" if windows else "probe.so")
    src.write_text("int probe(void) { return 0; }\n")
    t0 = time.monotonic()
    r = subprocess.run([*cc, "-shared", "-o", str(lib), str(src)], capture_output=True, **UTF8, check=False)
    print(f"link probe: {time.monotonic() - t0:.1f} s, rc {r.returncode}\n{r.stderr}", file=sys.stderr)


def tail_of(out: str | bytes | None) -> str:
    text = out.decode(errors="replace") if isinstance(out, bytes) else out or ""
    return text.strip()[-300:] or "no output"
