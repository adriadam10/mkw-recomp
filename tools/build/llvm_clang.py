"""LLVM's own clang of zig's LLVM version, for the package's setup/clang (tools/prepare/fastcc.py runs it).

Why: LLVM's release builds compile the game code's chunks ~30 % faster than zig cc's LLVM of the same version (see
fastcc.py). Only the clang binary is kept: zig's headers and its -cc1 line do the rest. The archives are large (Linux
1.9 GB, the Windows installer 0.46 GB), so each is read once, checked by sha256 as dist.py's downloads, and only the
binary stays in build/dist/dl/llvm-<version>-<target>/ (the Linux one stripped: 271 -> 236 MB).

Usage: imported by dist.py (clang(target)); by hand: uv run tools/build/llvm_clang.py linux|windows
"""

import hashlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DL = ROOT / "build/dist/dl"
VERSION = "22.1.8"  # zig 0.17.1's LLVM (`zig cc --version`); fastcc.py uses it only on a match
REL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{VERSION}/"
# target: (archive, its sha256 as GitHub's release lists it, the binary inside)
PINS = {
    "linux": (f"LLVM-{VERSION}-Linux-X64.tar.xz", "df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384",
              f"LLVM-{VERSION}-Linux-X64/bin/clang-22"),
    "windows": (f"LLVM-{VERSION}-win64.exe", "16e5709785fef73c854646241c4a92c5cd574318d1b33c63330dd7721903e55c",
                "bin/clang.exe"),
}  # fmt: skip


class Hashing:
    """A stream read through sha256, for tarfile's streaming mode."""

    def __init__(self, raw: object) -> None:
        self.raw, self.h = raw, hashlib.sha256()

    def read(self, n: int = -1) -> bytes:
        b = self.raw.read(n)
        self.h.update(b)
        return b

    def rest(self) -> str:
        while self.read(1 << 20):
            pass
        return self.h.hexdigest()


def clang(target: str) -> Path:
    """The cached binary for target (linux, windows), fetched and extracted the first time."""
    archive, want, member = PINS[target]
    out = DL / f"llvm-{VERSION}-{target}" / ("clang.exe" if target == "windows" else "clang")
    if out.is_file():
        return out
    out.parent.mkdir(parents=True, exist_ok=True)
    part = out.with_name(out.name + ".part")
    print(f"get {REL + archive}", flush=True)
    with urllib.request.urlopen(REL + archive) as r:
        if target == "linux":
            src = Hashing(r)
            with tarfile.open(fileobj=src, mode="r|xz") as tar:
                for m in tar:
                    if m.name == member:
                        with tar.extractfile(m) as f, part.open("wb") as o:
                            shutil.copyfileobj(f, o)
            got = src.rest()
        else:
            with tempfile.TemporaryDirectory(dir=DL) as tmp:
                exe = Path(tmp) / archive
                src = Hashing(r)
                with exe.open("wb") as o:
                    shutil.copyfileobj(src, o)
                got = src.h.hexdigest()
                if got == want:
                    # the installer is an NSIS archive: 7-Zip reads it (pacman 7zip; on Windows, 7-Zip's 7z.exe)
                    subprocess.run(
                        ["7z", "e", "-y", f"-o{tmp}", str(exe), member], check=True, stdout=subprocess.DEVNULL
                    )
                    shutil.move(Path(tmp) / Path(member).name, part)
    if got != want:
        part.unlink(missing_ok=True)
        sys.exit(f"{archive}: sha256 {got}, expected {want}")
    if not part.is_file():
        sys.exit(f"{archive}: no {member} in it")
    if target == "linux":
        subprocess.run(["strip", str(part)], check=True)
        part.chmod(0o755)
    part.rename(out)
    return out


if __name__ == "__main__":
    print(clang(sys.argv[1]))
