"""The game code's chunks compiled by the package's LLVM clang (setup/clang) on the -cc1 line zig cc would run.

Why: zig 0.17.1's LLVM 22.1.8 is built without profile-guided optimization. On a 3.5 MB chunk it took 4.95 s of CPU
against 3.37 s with LLVM's own 22.1.8 release on Linux, and 6.9 against 4.75 s on the 6-thread Windows PC (18/11/2026),
for the same machine code (objdump identical; only section order and the version string differ). zig cc -### prints
that line (zig's libc headers, the target, the flags); LLVM's clang runs it with each chunk's paths put in. Same
version, same line: the objects keep zig's key, so either compiler's objects are reused by the other.
zig compiles as before when the package has no clang, it does not start (a Linux older than glibc 2.34 or libstdc++
of GCC 12), its version is not zig's, or MKW_SETUP_CC=zig. zig still links. Not run on its own.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

PROBE = "fastcc_probe"
_QUOTED = re.compile(r'"((?:[^"\\]|\\.)*)"')
_ESCAPE = re.compile(r"\\(.)")
UTF8 = {"encoding": "utf-8", "errors": "replace"}


def clang_path(package: Path) -> Path:
    return package / "clang" / ("clang.exe" if os.name == "nt" else "clang")


def version(cmd: list[str]) -> str:
    """`clang version 22.1.8`, without what follows (LLVM's build adds its repository and commit, zig's nothing)."""
    try:
        r = subprocess.run([*cmd, "--version"], capture_output=True, **UTF8, check=False, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"did not start: {e}"
    m = re.search(r"clang version \S+", r.stdout)
    return m[0] if m else f"rc {r.returncode}: {(r.stdout + r.stderr).strip()[:200]}"


class Cc1:
    """clang -cc1 command lines, one per set of flags, from zig cc -###; cmd() gives a chunk's."""

    def __init__(self, clang: Path, zig_cc: list[str], obj: Path) -> None:
        self.clang, self.zig_cc = str(clang), zig_cc
        self.src, self.out = obj / f"{PROBE}.c", obj / f"{PROBE}.o"
        self.lines: dict[tuple[str, ...], list[str]] = {}
        self.outs: dict[tuple[str, ...], str] = {}  # the object path each line writes (zig.exe: a cache temporary)

    def add(self, flags: list[str]) -> None:
        """zig's -cc1 line for these flags; raises RuntimeError when it is not the expected one."""
        self.src.write_text("int fastcc_probe;\n")
        cmd = [*self.zig_cc, "-std=gnu11", *flags, "-c", str(self.src), "-o", str(self.out), "-###"]
        r = subprocess.run(cmd, capture_output=True, **UTF8, check=False, timeout=120)
        lines = [ln for ln in r.stderr.splitlines() if '"-cc1"' in ln]
        # zig.exe compiles into its cache's tmp/ and renames the object to -o afterwards; with -### there is no object,
        # so it prints the line and exits 1 on the rename (the Windows PC, 18/11/2026: every chunk fell back to zig)
        if len(lines) != 1 or (r.returncode and "failed to rename" not in r.stderr):
            raise RuntimeError(f"zig cc -### gave rc {r.returncode} and {len(lines)} -cc1 lines")
        args = [_ESCAPE.sub(r"\1", a) for a in _QUOTED.findall(lines[0])]
        out = args[args.index("-o") + 1] if "-o" in args[:-1] else ""
        # the probe's paths, each where the chunk's go: the output (ours or zig's temporary), the input and its name
        # (-main-file-name)
        if args[1] != "-cc1" or args.count(str(self.src)) != 1 or PROBE not in Path(out).name:
            raise RuntimeError("zig's -cc1 line does not name the probe's input once and an output of it")
        self.lines[tuple(flags)], self.outs[tuple(flags)] = args[1:], out

    def cmd(self, flags: list[str], src: Path, out: Path) -> list[str]:
        swap = {str(self.src): str(src), self.outs[tuple(flags)]: str(out), self.src.name: src.name}
        # zig.exe names the temporary as the object's CodeView name too
        return [
            self.clang,
            *(
                f"-object-file-name={out}" if a.startswith("-object-file-name=") else swap.get(a, a)
                for a in self.lines[tuple(flags)]
            ),
        ]


def setup(zig_cc: list[str], package: Path, obj: Path, flag_sets: list[list[str]]) -> Cc1 | None:
    """The Cc1 for these flag sets, or None (zig compiles) with the reason in setup.log."""
    clang = clang_path(package)
    why = ""
    if os.environ.get("MKW_SETUP_CC") == "zig":
        why = "MKW_SETUP_CC=zig"
    elif Path(zig_cc[0]).stem != "zig":
        why = f"the compiler is {zig_cc[0]}, not zig"
    elif not clang.is_file():
        why = f"no {clang}"
    else:
        have, want = version([str(clang)]), version(zig_cc)
        if have != want:
            why = f"{clang.name} is «{have}», zig's clang «{want}»"
    if why:
        print(f"compile: zig compiles ({why})", file=sys.stderr)
        return None
    cc1 = Cc1(clang, zig_cc, obj)
    try:
        for flags in flag_sets:
            cc1.add(flags)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"compile: zig compiles ({e})", file=sys.stderr)
        return None
    finally:
        cc1.src.unlink(missing_ok=True)
    print(f"compile: {clang} on zig's -cc1 lines", file=sys.stderr)
    return cc1
