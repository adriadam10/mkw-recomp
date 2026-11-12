"""The compile step of prepare.py: the generated C into mkwgame.so / mkwgame.dll with the package's zig (or clang).

Why apart: prepare.py holds the order of the steps and their keys; this step is the largest and has its own reuse of
earlier objects. Not run on its own.
"""

import hashlib
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]  # the repository, or the package's setup/src
sys.path.insert(0, str(ROOT / "tools/build"))
import build  # (GEN_FLAGS, gen_modules, module_glue, game_id: the same game code as build.py --dist)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from machine import Failed, crt_link, emit, package_dir

LINK_TIMEOUT = 30 * 60  # far past a slow link: only a hung linker (issue #2: "stuck" after the last object)


def compile_game(game: Path, exefs: Path, work: Path, cc: list[str], target: str, jobs: int) -> None:
    gen, obj = work / "generated", work / "obj"
    windows = target == "windows"
    flags = [*build.GEN_FLAGS, *([] if windows else ["-fPIC"])]
    if Path(cc[0]).stem == "zig":
        # zig cc turns -O1 into -O2 with both vectorizers, keeps frame pointers, adds debug info and UBSan, and spells
        # the CPU its own way: 182 126 instructions on a chunk clang -O1 makes in 150 969. With these, 150 745 (the
        # rest is LLVM 22 against 23), as SavageWindRecomp's GAMELIB_ZIGFLAGS (runtime/gamebuild.c, MIT).
        flags = [f.replace("-march=x86-64-v3", "-mcpu=x86_64_v3") for f in flags]
        flags += ["-Xclang", "-O1", "-fno-vectorize", "-fno-slp-vectorize", "-fomit-frame-pointer", "-g0",
                  "-fno-sanitize=all"]  # fmt: skip
    srcs = sorted(p for mod in build.gen_modules(gen) for p in (gen / mod).glob("*.c"))
    obj.mkdir(parents=True, exist_ok=True)
    (obj / "modules.c").write_text(build.module_glue(gen, exefs, True))
    gid = f'-DRT_GAME_ID="{build.game_id()}"'
    # every folder with a runtime header the package carries (runtime/cpu, runtime/core/boot for gamelib.h), as build.py
    # does: a fixed list missed gamelib.h when it moved to core/boot (issue #1, v0.1.1)
    inc = [f"-I{d}" for d in sorted({p.parent for p in (ROOT / "runtime").rglob("*.h")})] + [f"-I{gen}"]
    jobs_c = [(src, obj / f"{src.parent.name}_{src.stem}.o", [*flags, f"-I{ROOT / 'runtime/cpu'}"]) for src in srcs]
    jobs_c.append((obj / "modules.c", obj / "modules.o", [*flags, gid, *inc]))
    jobs_c.sort(key=lambda j: -j[0].stat().st_size)  # the largest first: no 10 MB main_table.c alone at the end
    total, done = len(jobs_c) + 1, 0  # the link is the last unit: a progress bar full before it hid it (issue #2)
    extra = [str(package_dir(game) / "lib/libmkw.dll.a")] if windows else ["-Wl,-z,noexecstack"]
    base = common_key(cc, gen)

    def one(job: tuple[Path, Path, list[str]]) -> str:
        src, out, f = job
        key = object_key(base, src, f, gen)
        stamp = out.with_suffix(".key")
        if out.is_file() and stamp.is_file() and stamp.read_text() == key:
            return ""
        stamp.unlink(missing_ok=True)
        r = subprocess.run(
            [*cc, "-std=gnu11", *f, "-c", str(src), "-o", str(out)], capture_output=True, text=True, check=False
        )
        if r.returncode:
            return f"{src.name}: {r.stderr.strip()[-300:]}"
        stamp.write_text(key)
        return ""

    lib = game / ("mkwgame.dll" if windows else "mkwgame.so")
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
    objs = [str(o) for _, o, _ in jobs_c]
    for stale in set(obj.glob("*.o")) - {o for _, o, _ in jobs_c}:  # chunks an earlier version had
        stale.unlink()
        stale.with_suffix(".key").unlink(missing_ok=True)
    rsp.write_text("\n".join(f'"{o}"' for o in objs).replace("\\", "/"))
    t0 = time.monotonic()
    try:
        r = subprocess.run(link or [*cc, "-shared", "-o", str(part), f"@{rsp}", *extra], capture_output=True,
                           text=True, check=False, timeout=LINK_TIMEOUT)  # fmt: skip
    except subprocess.TimeoutExpired as e:
        raise Failed(f"linking the game code took over {LINK_TIMEOUT // 60} min: {tail_of(e.stderr)}") from None
    print(f"link: {time.monotonic() - t0:.1f} s, rc {r.returncode}\n{r.stderr}", file=sys.stderr)
    if r.returncode:
        raise Failed(f"linking the game code failed: {r.stderr.strip()[-300:]}")
    part.replace(lib)
    emit("progress", total, total)
    (game / "mkwgame.id").write_text(build.game_id())  # runtime/core/boot/gamelib.c rt_game_present


def common_key(cc: list[str], gen: Path) -> str:
    """What every object depends on besides its own C and flags: the compiler and every header it can include (the
    runtime's and the generated ones)."""
    h = hashlib.sha256(subprocess.run([*cc, "--version"], capture_output=True, text=True, check=False).stdout.encode())
    for p in sorted((ROOT / "runtime").rglob("*.h")) + sorted(gen.glob("*.h")):
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
    r = subprocess.run([*cc, "-shared", "-o", str(lib), str(src)], capture_output=True, text=True, check=False)
    print(f"link probe: {time.monotonic() - t0:.1f} s, rc {r.returncode}\n{r.stderr}", file=sys.stderr)


def tail_of(out: str | bytes | None) -> str:
    text = out.decode(errors="replace") if isinstance(out, bytes) else out or ""
    return text.strip()[-300:] or "no output"

