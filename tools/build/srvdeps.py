"""Fetch the sources of the online server's two libraries into build/srv and say how to compile them: nghttp2 (MIT,
HTTP/2) and Mbed TLS (Apache-2.0, TLS and the certificates the server makes for itself), linked statically into the
executable (runtime/srv/).

Why: the server built into mkw.exe (mkw.exe --server) needs nothing installed beside the game; the game
speaks gRPC over HTTP/2 over TLS and both libraries are plain C with no build step of their own needed: their release
archives carry their generated files, so build.py compiles their sources like the runtime's. Every download is checked
against its sha256 (the ones each project publishes beside its release); a mismatch stops.

Layout:
    build/srv/dl/     the archives
    build/srv/src/    nghttp2-<v>/ and mbedtls-<v>/, unpacked once (a stamp holds the archive's sha256)

Usage: uv run tools/build/srvdeps.py [--dry-run]   (build.py runs it when build/srv is missing; ~10 MB)
"""

import argparse
import hashlib
import shutil
import sys
import tarfile
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRV = ROOT / "build/srv"
NGHTTP2 = "1.71.0"
MBEDTLS = "4.1.1"  # the 4.1 long-term-support branch (supported until March 2029)
# name: (url, sha256, folder in the archive)
PINS = {
    "nghttp2": (
        f"https://github.com/nghttp2/nghttp2/releases/download/v{NGHTTP2}/nghttp2-{NGHTTP2}.tar.gz",
        "0cdd35ecec77919b4b7b245f87fe8d2216e3d62da854eb0838c594ff058042fc",
        f"nghttp2-{NGHTTP2}",
    ),
    "mbedtls": (
        f"https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-{MBEDTLS}/mbedtls-{MBEDTLS}.tar.bz2",
        "3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c",
        f"mbedtls-{MBEDTLS}",
    ),
}


@dataclass
class Deps:
    """What build.py needs: each library's sources with its own flags, and the include folders the runtime uses."""

    groups: dict[str, tuple[list[Path], list[str]]] = field(default_factory=dict)  # name: (sources, flags)
    includes: list[Path] = field(default_factory=list)
    defines: list[str] = field(default_factory=list)  # for the runtime's sources too
    licences: dict[str, Path] = field(default_factory=dict)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(name: str) -> Path:
    url, want, _ = PINS[name]
    path = SRV / "dl" / url.rsplit("/", 1)[1]
    if not path.exists() or sha256(path) != want:
        print(f"get {url}")
        path.parent.mkdir(parents=True, exist_ok=True)
        part = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(url) as r, part.open("wb") as f:
            shutil.copyfileobj(r, f)
        part.replace(path)
    got = sha256(path)
    if got != want:
        sys.exit(f"{path.name}: sha256 {got}, expected {want}")
    return path


def unpack(name: str) -> Path:
    """The library's folder in build/srv/src, unpacked from its checked archive unless the stamp says it already is."""
    _, want, folder = PINS[name]
    dest = SRV / "src" / folder
    stamp = dest / ".sha256"
    if stamp.exists() and stamp.read_text().strip() == want:
        return dest
    archive = fetch(name)
    tmp = SRV / "src" / (folder + ".tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    with tarfile.open(archive) as t:
        t.extractall(tmp, filter="data")  # no absolute paths, no "..", no links out of the folder
    shutil.rmtree(dest, ignore_errors=True)
    (tmp / folder).replace(dest)
    shutil.rmtree(tmp, ignore_errors=True)
    stamp.write_text(want + "\n")
    return dest


def ensure(windows: bool) -> Deps:
    ng, mb = unpack("nghttp2"), unpack("mbedtls")
    tf = mb / "tf-psa-crypto"
    deps = Deps()
    ng_flags = [f"-I{ng / 'lib/includes'}", "-DNGHTTP2_STATICLIB", "-DBUILDING_NGHTTP2"]
    ng_flags += (
        ["-DHAVE_WINDOWS_H", "-DHAVE_GETTICKCOUNT64"]
        if windows
        else ["-DHAVE_ARPA_INET_H", "-DHAVE_NETINET_IN_H", "-DHAVE_CLOCK_GETTIME", "-DHAVE_DECL_CLOCK_MONOTONIC=1"]
    )
    deps.groups["nghttp2"] = (sorted((ng / "lib").glob("*.c")), ng_flags)
    public = [mb / "include", tf / "include", tf / "drivers/builtin/include"]
    private = [mb / "library", tf / "core", tf / "dispatch", tf / "extras", tf / "platform", tf / "utilities"]
    private.append(tf / "drivers/builtin/src")
    mb_srcs = [mb / "library", *(tf / d for d in ("core", "platform", "utilities", "extras", "drivers/builtin/src"))]
    deps.groups["mbedtls"] = (sorted(p for d in mb_srcs for p in d.glob("*.c")), [f"-I{d}" for d in public + private])
    deps.includes = [ng / "lib/includes", *public]
    deps.defines = ["-DNGHTTP2_STATICLIB"]
    deps.licences = {"nghttp2": ng / "COPYING", "mbedtls": mb / "LICENSE"}
    return deps


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dry-run", action="store_true", help="list the pinned downloads and stop")
    args = ap.parse_args()
    if args.dry_run:
        for name, (url, want, _) in PINS.items():
            print(f"{name}: {url} {want}")
        return
    deps = ensure(sys.platform == "win32")
    print(", ".join(f"{name} {len(srcs)} sources" for name, (srcs, _) in deps.groups.items()))


if __name__ == "__main__":
    main()
