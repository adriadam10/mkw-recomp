"""Names the host addresses of build/mkw that a run's log prints (exe+0x..., mkw+0x..., build/mkw+0x..., 0x...).

Why: build/mkw is not position independent and loads at 0x200000, and the log gives offsets from that base; the
sessions to 01/11/2026 named addresses by hand 29 times, adding (or forgetting) the base. A lifted function f_<addr>
is also named in the guest (main+0x... and its symbol) through tools/analisis/bin/q.py.

Usage: uv run tools/run/sym.py exe+0x121f3c6 [0x1220b16 ...]   (bare hex: an offset from the base, like exe+)
       uv run tools/run/sym.py < build/run/NAME.log      every such address in the text (first 40, unique)
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "build/mkw"
BASE = 0x200000  # first PT_LOAD of build/mkw (readelf -l)
TOKEN = re.compile(r"\b(?:(exe|build/mkw|mkw)\+)?(0x[0-9a-f]+)\b")


def addresses(text: str, bare: bool) -> list[tuple[str, int]]:
    """(token, absolute address); bare hex only from the command line, where it is an offset like exe+."""
    out: dict[str, int] = {}
    for m in TOKEN.finditer(text):
        if m[1] or bare:
            out.setdefault(m[0], BASE + int(m[2], 16))
    return list(out.items())[:40]


def guest(name: str) -> str:
    m = re.fullmatch(r"f_([0-9a-f]+)", name)
    if not m:
        return ""
    q = subprocess.run(
        [sys.executable, str(ROOT / "tools/analisis/bin/q.py"), "at", "0x" + m[1]],
        check=False,
        capture_output=True,
        text=True,
    )
    return q.stdout.strip().splitlines()[0] if q.returncode == 0 and q.stdout.strip() else ""


def main() -> None:
    args = sys.argv[1:]
    if args in ([], ["-"]) and sys.stdin.isatty():
        sys.exit(__doc__)
    found = addresses(" ".join(args), bare=True) if args else addresses(sys.stdin.read(), bare=False)
    if not found:
        sys.exit("sym.py: no host address in the input")
    res = subprocess.run(
        ["addr2line", "-f", "-i", "-C", "-a", "-e", str(EXE), *(hex(a) for _, a in found)],
        capture_output=True,
        text=True,
        check=True,
    )
    blocks = re.split(r"^0x[0-9a-f]+\n", res.stdout, flags=re.MULTILINE)[1:]
    for (tok, addr), block in zip(found, blocks, strict=True):
        lines = block.splitlines()
        frames = [f"{fn} ({Path(loc).name})" for fn, loc in zip(lines[::2], lines[1::2], strict=False)]
        print(f"{tok} = {addr:#x}: {' < '.join(frames)}")
        if frames and (g := guest(lines[0])):
            print(f"    guest: {g}")


if __name__ == "__main__":
    main()
