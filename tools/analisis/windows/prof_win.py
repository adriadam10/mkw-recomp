#!/usr/bin/env python3
"""Report of a Windows sampling profile (MKW_PROFILE=<path>, runtime/core/diag/prof_win.c): samples per thread, per module
and per function. Functions of mkw.exe and mkwgame.dll are named with llvm-symbolizer (llvm-mingw of build/win) from the
binaries of build/dist/windows; other modules (the NVIDIA driver, ntdll, kernel32) are summed by module.
Slow-frame dumps (MKW_PROFILE_SLOW=<ms>: PROFILE.f<frame>, `tstack` lines with the thread) are summed per thread
instead: for each thread its samples, the functions it was in (leaf) and the first frame of ours under a driver or
system call, over every dump given.

Usage: uv run tools/analisis/windows/prof_win.py PROFILE [--top 40] [--bin-dir build/dist/windows]
       uv run tools/analisis/windows/prof_win.py PROFILE.f* [--top 8]
"""

import argparse
import bisect
import subprocess
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ROLES = {1: "main", 2: "render", 3: "replay", 0: "other"}
SYMBOLIZER = ROOT / "build/win/llvm-mingw/bin/llvm-symbolizer"
OBJDUMP = ROOT / "build/win/llvm-mingw/bin/llvm-objdump"


def image_base(path: Path) -> int:
    """ImageBase of a PE (the address the symbolizer expects)."""
    out = subprocess.run([OBJDUMP, "-p", path], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        if line.startswith("ImageBase"):
            return int(line.split()[1], 16)
    raise SystemExit(f"no ImageBase in {path}")


def symbolize(path: Path, addrs: list[int]) -> dict[int, str]:
    """Function name of each file address (one llvm-symbolizer run for all of them)."""
    if not addrs:
        return {}
    res = subprocess.run(
        [SYMBOLIZER, "--obj", path, "--functions=short", "--no-inlines", "--output-style=GNU"],
        input="".join(f"0x{a:x}\n" for a in addrs), capture_output=True, text=True, check=True,
    ).stdout.split("\n")
    # GNU style: two lines per address (function, file:line)
    return {a: res[2 * i] for i, a in enumerate(addrs)}


class Names:
    """Names of runtime addresses: our functions by symbol, other modules by file name."""

    def __init__(self, modules: list[tuple[int, int, str]], bin_dir: Path) -> None:
        self.modules = sorted(modules)
        self.starts = [m[0] for m in self.modules]
        self.bin_dir = bin_dir
        self.cache: dict[int, str] = {}

    def module(self, rip: int) -> tuple[int, str] | None:
        i = bisect.bisect_right(self.starts, rip) - 1
        if i < 0 or rip >= self.modules[i][0] + self.modules[i][1]:
            return None
        return self.modules[i][0], Path(self.modules[i][2].replace("\\", "/")).name

    def ours(self, rip: int) -> bool:
        m = self.module(rip)
        return bool(m) and m[1].lower() in ("mkw.exe", "mkwgame.dll")

    def resolve(self, rips: set[int]) -> None:
        per: dict[str, list[int]] = {}
        for r in rips:
            m = self.module(r)
            if not m:
                self.cache[r] = "?"
            elif m[1].lower() in ("mkw.exe", "mkwgame.dll"):
                per.setdefault(m[1].lower(), []).append(r)
            else:
                self.cache[r] = f"[{m[1]}]"
        for name, rs in per.items():
            path = self.bin_dir / name
            base = image_base(path)
            mb = self.module(rs[0])[0]
            names = symbolize(path, sorted({r - mb + base for r in rs}))
            for r in rs:
                self.cache[r] = names.get(r - mb + base, "?")


def per_thread(paths: list[Path], bin_dir: Path, top: int) -> None:
    """Slow-frame dumps: samples per thread, leaf functions and our first frame, summed over the dumps."""
    modules: list[tuple[int, int, str]] = []
    roles: dict[int, str] = {}
    stacks: list[tuple[int, list[int]]] = []
    frames = []
    for p in paths:
        for line in p.read_text().splitlines():
            w = line.split()
            if not w:
                continue
            if w[0] == "module" and not modules_seen(modules, w):
                modules.append((int(w[1], 16), int(w[2], 16), " ".join(w[3:])))
            elif w[0] == "frame":
                frames.append(float(w[3]))
            elif w[0] in ("main", "render", "replay"):
                roles.update({int(w[i + 1]): w[i] for i in range(0, len(w) - 1, 2)})
            elif w[0] == "tstack":
                stacks.append((int(w[1]), [int(x, 16) for x in w[3:]]))
    names = Names(modules, bin_dir)
    names.resolve({r for _, st in stacks for r in st})
    leaf: dict[int, Counter[str]] = {}
    ours: dict[int, Counter[str]] = {}
    for tid, st in stacks:
        if not st:
            continue
        leaf.setdefault(tid, Counter())[names.cache[st[0]]] += 1
        first = next((r for r in st if names.ours(r)), None)
        ours.setdefault(tid, Counter())[names.cache[first] if first else "-"] += 1
    print(f"{len(paths)} dumps, frames of {min(frames):.0f}-{max(frames):.0f} ms (mean {sum(frames) / len(frames):.0f})")
    for tid, c in sorted(leaf.items(), key=lambda kv: -sum(kv[1].values()))[:10]:
        n = sum(c.values())
        print(f"\nthread {tid} {roles.get(tid, '')}: {n} samples ({n / len(paths):.0f} per dump)")
        for name, k in c.most_common(top):
            print(f"  leaf {100 * k / n:5.1f}%  {name[:120]}")
        for name, k in ours[tid].most_common(top):
            print(f"  ours {100 * k / n:5.1f}%  {name[:120]}")


def modules_seen(modules: list[tuple[int, int, str]], w: list[str]) -> bool:
    return any(m[0] == int(w[1], 16) for m in modules)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("profile", type=Path, nargs="+")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--bin-dir", type=Path, default=ROOT / "build/dist/windows")
    a = ap.parse_args()
    if any(".f" in p.suffix for p in a.profile):
        per_thread(a.profile, a.bin_dir, a.top)
        return
    a.profile = a.profile[0]

    threads, modules, rips, roles, chains = [], [], [], {}, []
    total = 0
    for line in a.profile.read_text().splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "samples":
            total = int(w[1])
        elif w[0] == "thread":
            threads.append((int(w[1]), int(w[2]), int(w[3])))
        elif w[0] == "module":
            modules.append((int(w[1], 16), int(w[2], 16), " ".join(w[3:])))
        elif w[0] == "rip":   # a fourth field: the thread role (1 main, 2 render, 3 replay, 0 other)
            rips.append((int(w[1], 16), int(w[2]), int(w[3]) if len(w) > 3 else 0))
        elif w[0] in ("main", "render", "replay"):
            roles.update({int(w[i + 1]): w[i] for i in range(0, len(w) - 1, 2)})
        elif w[0] == "stack":   # MKW_PROFILE_CHAINS / _RENDER: count, then the chain from the sampled Rip
            chains.append((int(w[1]), [int(x, 16) for x in w[2:]]))
    modules.sort()
    starts = [m[0] for m in modules]
    n_rip = sum(n for _, n, _ in rips)

    print(f"{a.profile}: {total} samples, {n_rip} in the top Rips, {len(threads)} threads")
    print("\nthreads (samples, of them in mkw.exe):")
    for tid, n, ex in sorted(threads, key=lambda t: -t[1])[:12]:
        print(f"  {tid:>7} {roles.get(tid, ''):7} {n:>8} {ex:>8}")

    by_mod: Counter[str] = Counter()
    by_role_mod: dict[int, Counter[str]] = {}
    per_bin: dict[str, list[tuple[int, int, int]]] = {}
    for rip, n, role in rips:
        i = bisect.bisect_right(starts, rip) - 1
        if i < 0 or rip >= modules[i][0] + modules[i][1]:
            by_mod["?"] += n
            continue
        name = Path(modules[i][2].replace("\\", "/")).name
        by_mod[name] += n
        if name.lower() in ("mkw.exe", "mkwgame.dll"):
            per_bin.setdefault(name.lower(), []).append((rip - modules[i][0], n, role))
        else:
            per_bin.setdefault("", []).append((0, n, role))
            by_role_mod.setdefault(role, Counter())[name] += n
    print("\nmodules:")
    for name, n in by_mod.most_common(15):
        print(f"  {100 * n / n_rip:5.1f}%  {name}")

    by_fn: Counter[str] = Counter()
    by_role: dict[int, Counter[str]] = {}
    for name, hits in per_bin.items():
        if not name:
            continue
        path = a.bin_dir / name
        base = image_base(path)
        names = symbolize(path, sorted({off + base for off, _, _ in hits}))
        for off, n, role in hits:
            fn = f"{names.get(off + base, '?')} [{name}]"
            by_fn[fn] += n
            by_role.setdefault(role, Counter())[fn] += n
    for role, mods in by_role_mod.items():
        for mod, n in mods.items():
            by_role.setdefault(role, Counter())[f"[{mod}]"] += n
    print("\nfunctions of mkw.exe and mkwgame.dll:")
    for fn, n in by_fn.most_common(a.top):
        print(f"  {100 * n / n_rip:5.1f}%  {fn[:150]}")
    if chains:   # system time by the first function of ours under it: what calls into the OS and the driver
        names = Names(modules, a.bin_dir)
        names.resolve({r for _, st in chains for r in st})
        sys_by: Counter[str] = Counter()
        n_sys = 0
        for n, st in chains:
            if not st or names.ours(st[0]):
                continue
            n_sys += n
            first = next((r for r in st if names.ours(r)), None)
            sys_by[f"{names.cache[st[0]]} <- {names.cache[first] if first else '-'}"] += n
        n_ch = sum(n for n, _ in chains)
        print(f"\nchains: {n_ch} samples, {n_sys} outside our code; by module and first function of ours:")
        for k, n in sys_by.most_common(a.top):
            print(f"  {100 * n / n_ch:5.1f}%  {k[:140]}")
    for role in sorted(by_role, key=lambda r: (r == 0, r)):
        c = by_role[role]
        n_r = sum(c.values())
        print(f"\n{ROLES[role]} thread{'s' if role == 0 else ''}: {n_r} samples (functions; other modules in [ ])")
        for fn, n in c.most_common(min(a.top, 15)):
            print(f"  {100 * n / n_r:5.1f}%  {fn[:150]}")


if __name__ == "__main__":
    main()
