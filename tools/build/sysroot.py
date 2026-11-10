"""Unpack the Steam Runtime 3 "sniper" SDK sysroot into build/sysroot, for a Linux release that starts on older systems.

Why: the runtime built against the build machine's own glibc needs that glibc or newer (Arch on 09/11/2026: 2.43, which
Ubuntu 24.04's 2.39 lacks). Built with the system clang against this sysroot (glibc 2.31, SDL3 3.4, Opus, libvpx 6)
through build.py --sysroot, it runs on SteamOS and on any distribution at least as new as Debian 11. Same approach and
the same pinned snapshot as totk-pc-recomp's tools/make_release_linux.py (MIT).

Besides the sniper files, it adds under build/sysroot/usr/local what the SDK lacks, built with that same clang: zstd
1.5.7 as a static library (the SDK has no zstd headers), the xxHash 0.8.3 header, and SDL 3.4.18 in place of the SDK's,
which links X11, Wayland, PulseAudio, ALSA and libdecor directly (the Steam Runtime brings them, a desktop not always):
ours loads them at run time, as SDL does by default, and needs only glibc. Every download pinned by sha256.

AArch64 (--arch aarch64, into build/sysroot-aarch64): Valve publishes no sniper for it, so the same glibc 2.31 comes
from Debian 11 (bullseye) arm64: the packages of ARM_SEEDS and the libraries and headers they depend on, from the pinned
archive.debian.org indexes (main, then bullseye-security over it), each .deb checked against its index. Debian 11's
Vulkan headers are 1.2.162: Khronos' Vulkan-Headers go to usr/local/include. SDL's Wayland code is generated with
bullseye's own wayland-scanner (the amd64 package, run here with its libraries), for the reason given in build_sdl3.

Usage: uv run tools/build/sysroot.py [--arch x86_64|aarch64]   (x86_64: ~1.2 GB download the first time, ~3 min;
       aarch64: ~150 MB; both need clang, lld, make, cmake, pkgconf)
"""

import argparse
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dist import DL, fetch  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SYSROOT = ROOT / "build/sysroot"
SNIPER = "3.0.20261021.266025"
PINS = {
    "sniper": (f"https://repo.steampowered.com/steamrt-images-sniper/snapshots/{SNIPER}/"
               "com.valvesoftware.SteamRuntime.Sdk-amd64%2Ci386-sniper-sysroot.tar.gz",
               "8f71947035f97925a311b2ed753f22efcf2f13d6f1631945b8e537f8903ce2cf"),
    "zstd": ("https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz",
             "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3"),
    "xxhash": ("https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.3/xxhash.h",
               "17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b"),
    "sdl3": ("https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz",
             "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"),
    "vulkan-headers": ("https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.368.tar.gz",
                       "5c5c6f64fcce28a24cc934143a048707af036a858e4ed11d7f71fe016006ecda"),
}  # fmt: skip
# AArch64: Debian 11 from archive.debian.org (bullseye's final state, 11.11 and its last security updates),
# (archive, distribution/component, architecture) -> Packages.xz sha256.
DEB_SNAPSHOT = "11.11-final"
DEB_INDEXES = {
    ("debian", "bullseye/main", "arm64"): "71a52585dc5ca78e5d682e13fc702a077c2b14355bce020ab0ff94e4ef84107a",
    ("debian-security", "bullseye-security/main", "arm64"): "5624f634ddd2daa0ac2c32dfd3baf289a1d7a5e46cee1edf58735b0d7340bdae",
    ("debian", "bullseye/main", "amd64"): "1c3405444297ead864711c18e64bce4810db63b48340a2f9d81182d9f235ad71",
    ("debian-security", "bullseye-security/main", "amd64"): "f8863ad9e81b60ff7056c01764e15c3c6ec9e144ab6874304d0e4349af1a5fb1",
}  # fmt: skip
# What a compile and link of the runtime and of SDL3 read (SDL loads the libraries at run time: headers are enough).
ARM_SEEDS = ["libc6-dev", "libgcc-10-dev", "libstdc++-10-dev", "libopus-dev", "libvpx-dev", "libx11-dev", "libxext-dev",
             "libxrandr-dev", "libxcursor-dev", "libxi-dev", "libxfixes-dev", "libxss-dev", "libxtst-dev",
             "libxkbcommon-dev", "libwayland-dev", "wayland-protocols", "libegl-dev", "libpulse-dev", "libasound2-dev",
             "libpipewire-0.3-dev", "libdbus-1-dev", "libudev-dev", "libdrm-dev", "libgbm-dev", "libvulkan-dev"]  # fmt: skip
ARCHES = {  # build.py's --arch -> sysroot folder, clang triple, Debian multiarch directory, dynamic loader
    "x86_64": (ROOT / "build/sysroot", "x86_64-linux-gnu", "ld-linux-x86-64.so.2"),
    "aarch64": (ROOT / "build/sysroot-aarch64", "aarch64-linux-gnu", "ld-linux-aarch64.so.1"),
}
# Libraries ours may need at load time: SDL3 has to load everything else itself.
SDL_NEEDED = {"libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "ld-linux-x86-64.so.2",
              "ld-linux-aarch64.so.1"}
# The parts of the SDK a C/C++ compile and link for x86_64 read, and the licences of the libraries a release bundles.
MEMBERS = ["usr/include", "usr/lib/x86_64-linux-gnu", "usr/lib/gcc/x86_64-linux-gnu", "lib/x86_64-linux-gnu", "lib64",
           "usr/share/pkgconfig", "usr/share/wayland-protocols", "usr/bin/wayland-scanner", "usr/share/doc/libopus0",
           "usr/share/doc/libvpx6"]  # fmt: skip
STAMP = SYSROOT / f".sniper-{SNIPER}"


TRIPLE = "x86_64-linux-gnu"


def use_arch(arch: str) -> None:
    """The sysroot the functions below work on (release.py reads SYSROOT and TRIPLE after this too)."""
    global SYSROOT, TRIPLE, STAMP
    SYSROOT, TRIPLE, _ = ARCHES[arch]
    STAMP = SYSROOT / (f".sniper-{SNIPER}" if arch == "x86_64" else f".bullseye-{DEB_SNAPSHOT}")


def cc() -> str:
    return f"clang --target={TRIPLE} --sysroot={SYSROOT} -fuse-ld=lld"


def unpack_sniper() -> None:
    tarball = fetch(*PINS["sniper"])
    shutil.rmtree(SYSROOT, ignore_errors=True)
    SYSROOT.mkdir(parents=True)
    # the system tar extracts only the members, the rest of the 1.2 GB stays packed
    subprocess.run(["tar", "-xzf", str(tarball), "-C", str(SYSROOT), "--wildcards", "--no-wildcards-match-slash",
                    *MEMBERS], check=True)  # fmt: skip
    # the SDK points some links at /lib and /usr/lib, which outside its container are this machine's
    for p in SYSROOT.rglob("*"):
        if p.is_symlink() and os.readlink(p).startswith("/"):
            target = SYSROOT / os.readlink(p).lstrip("/")
            p.unlink()
            p.symlink_to(os.path.relpath(target, p.parent))


def add_deps() -> None:
    local = SYSROOT / "usr/local"
    (local / "include").mkdir(parents=True, exist_ok=True)
    (local / "lib").mkdir(parents=True, exist_ok=True)
    shutil.copy2(fetch(*PINS["xxhash"]), local / "include/xxhash.h")
    src = DL / "zstd-1.5.7"
    shutil.rmtree(src, ignore_errors=True)
    subprocess.run(["tar", "-xzf", str(fetch(*PINS["zstd"])), "-C", str(DL)], check=True)
    lib = src / "lib"
    subprocess.run(["make", "-C", str(lib), f"-j{os.cpu_count() or 4}", "libzstd.a", f"CC={cc()}", "AR=llvm-ar",
                    "ZSTD_LEGACY_SUPPORT=0", "MOREFLAGS=-O2 -fPIC"], check=True, stdout=subprocess.DEVNULL)  # fmt: skip
    shutil.copy2(lib / "libzstd.a", local / "lib/libzstd.a")
    for name in ("zstd.h", "zstd_errors.h"):
        shutil.copy2(lib / name, local / "include" / name)
    (local / "share/doc/zstd").mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "LICENSE", local / "share/doc/zstd/LICENSE")
    shutil.rmtree(src)


def build_sdl3() -> None:
    """SDL3 into usr/local, the SDK's removed so that no compile or link finds it instead."""
    sdk = SYSROOT / f"usr/lib/{TRIPLE}"
    for p in [*sdk.glob("libSDL3.*"), *sdk.glob("libSDL3_test.*"), sdk / "pkgconfig/sdl3.pc"]:
        p.unlink(missing_ok=True)
    shutil.rmtree(SYSROOT / "usr/include/SDL3", ignore_errors=True)
    src = DL / "SDL3-3.4.18"
    shutil.rmtree(src, ignore_errors=True)
    subprocess.run(["tar", "-xzf", str(fetch(*PINS["sdl3"])), "-C", str(DL)], check=True)
    local = SYSROOT / "usr/local"
    # The SDK's wayland-scanner: a newer one writes calls its libwayland 1.18 lacks (wl_proxy_marshal_flags), which
    # Debian 11's lacks too. It runs here with the SDK's libraries (libxml2), never its glibc (lib/, not usr/lib/).
    scanner = local / "bin/wayland-scanner"
    scanner.parent.mkdir(parents=True, exist_ok=True)
    host = SYSROOT / "host" if TRIPLE != "x86_64-linux-gnu" else SYSROOT   # aarch64: bullseye's amd64 scanner
    scanner.write_text(f'#!/bin/sh\nLD_LIBRARY_PATH="{host}/usr/lib/x86_64-linux-gnu:{host}/lib/x86_64-linux-gnu" '
                       f'exec "{host}/usr/bin/wayland-scanner" "$@"\n')  # fmt: skip
    scanner.chmod(0o755)
    pc = [SYSROOT / f"usr/lib/{TRIPLE}/pkgconfig", SYSROOT / "usr/share/pkgconfig"]
    env = dict(os.environ, PKG_CONFIG_SYSROOT_DIR=str(SYSROOT), PKG_CONFIG_PATH="",
               PKG_CONFIG_LIBDIR=os.pathsep.join(map(str, pc)))  # fmt: skip
    subprocess.run(["cmake", "-S", str(src), "-B", str(src / "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_C_COMPILER=clang", f"-DCMAKE_C_COMPILER_TARGET={TRIPLE}",
                    "-DCMAKE_SYSTEM_NAME=Linux", f"-DCMAKE_SYSTEM_PROCESSOR={TRIPLE.split('-')[0]}",
                    f"-DCMAKE_SYSROOT={SYSROOT}", "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld",
                    # headers and libraries only from the sysroot (this machine's liburing.h was picked up)
                    "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY", "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
                    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY", "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
                    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld", f"-DCMAKE_INSTALL_PREFIX={local}",
                    "-DCMAKE_INSTALL_LIBDIR=lib", "-DSDL_STATIC=OFF", "-DSDL_TEST_LIBRARY=OFF", "-DSDL_TESTS=OFF",
                    "-DSDL_EXAMPLES=OFF", "-DSDL_RPATH=OFF", "-DSDL_DEPS_SHARED=ON",
                    f"-DWAYLAND_SCANNER={scanner}"],
                   env=env, check=True, stdout=subprocess.DEVNULL)  # fmt: skip
    summary = subprocess.run(["cmake", "-L", "-N", str(src / "build")], capture_output=True, text=True).stdout
    print(" ".join(line for line in summary.splitlines() if line.startswith(("SDL_X11:", "SDL_WAYLAND:",
          "SDL_PULSEAUDIO:", "SDL_PIPEWIRE:", "SDL_ALSA:", "SDL_VULKAN:"))))  # fmt: skip
    subprocess.run(["cmake", "--build", str(src / "build")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["cmake", "--install", str(src / "build")], check=True, stdout=subprocess.DEVNULL)
    (local / "share/doc/SDL3").mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "LICENSE.txt", local / "share/doc/SDL3/LICENSE.txt")
    shutil.rmtree(src)
    lib = (local / "lib/libSDL3.so.0").resolve()
    dyn = subprocess.run(["readelf", "-d", str(lib)], capture_output=True, text=True, check=True).stdout
    extra = {line.split("[", 1)[1].rstrip("]") for line in dyn.splitlines() if "(NEEDED)" in line} - SDL_NEEDED
    if extra:
        sys.exit(f"{lib} links {' '.join(sorted(extra))}: their SDL_*_SHARED options are off")


# ---- AArch64: Debian 11 packages
def deb_index(archive: str, dist: str, arch: str) -> dict[str, dict[str, str]]:
    url = f"https://archive.debian.org/{archive}/dists/{dist}/binary-{arch}/Packages.xz"
    path = fetch(url, DEB_INDEXES[(archive, dist, arch)], f"Packages-{archive}-{dist.split('/')[0]}-{arch}.xz")
    out = {}
    for block in lzma.decompress(path.read_bytes()).decode().split("\n\n"):
        f, key = {}, None
        for line in block.splitlines():
            if line[:1] in (" ", "\t") and key:
                f[key] += "\n" + line
            elif ":" in line:
                key, v = line.split(":", 1)
                f[key] = v.strip()
        if "Package" in f:
            f["archive"] = archive
            out[f["Package"]] = f
    return out


def deb_packages(arch: str) -> dict[str, dict[str, str]]:
    """bullseye main, with bullseye-security's newer builds over it."""
    pk = deb_index("debian", "bullseye/main", arch)
    pk.update(deb_index("debian-security", "bullseye-security/main", arch))
    return pk


def deb_closure(pk: dict[str, dict[str, str]], seeds: list[str]) -> list[str]:
    """seeds and what they depend on, libraries, headers and data only (no interpreters or tools)."""
    provides = {}
    for name, f in pk.items():
        for p in f.get("Provides", "").split(","):
            provides.setdefault(p.split("(")[0].strip(), name)

    def keep(n: str) -> bool:
        return (n.startswith("lib") or n.endswith("-dev") or n in ("linux-libc-dev", "wayland-protocols")) and \
            not n.startswith(("libperl", "libpython", "libglib2.0-dev-bin", "libc-dev-bin", "libc-bin"))

    done, todo = set(), list(seeds)
    while todo:
        n = todo.pop()
        n = n if n in pk else provides.get(n, n)
        if n in done or n not in pk:
            continue
        done.add(n)
        for field in ("Pre-Depends", "Depends"):
            for alt in pk[n].get(field, "").split(","):
                for choice in alt.split("|"):
                    d = choice.split("(")[0].split(":")[0].strip()
                    d = d if d in pk else provides.get(d, "")
                    if d:
                        if keep(d) or d in seeds:
                            todo.append(d)
                        break
    return sorted(done)


def deb_extract(deb: Path, dst: Path) -> None:
    data = deb.read_bytes()
    at = 8   # "!<arch>\n", then 60-byte member headers
    while at < len(data):
        name, size = data[at:at + 16].decode().strip().rstrip("/"), int(data[at + 48:at + 58])
        body = data[at + 60:at + 60 + size]
        if name.startswith("data.tar"):
            import io

            with tarfile.open(fileobj=io.BytesIO(body)) as t:
                t.extractall(dst, filter="tar")
            return
        at += 60 + size + (size & 1)
    sys.exit(f"{deb}: no data.tar")


def deb_install(pk: dict[str, dict[str, str]], names: list[str], dst: Path) -> None:
    for n in names:
        f = pk[n]
        deb = fetch(f"https://archive.debian.org/{f['archive']}/{f['Filename']}", f["SHA256"])
        deb_extract(deb, dst)
    print(f"{len(names)} packages into {dst}")


def unpack_bullseye() -> None:
    shutil.rmtree(SYSROOT, ignore_errors=True)
    SYSROOT.mkdir(parents=True)
    arm = deb_packages("arm64")
    deb_install(arm, deb_closure(arm, ARM_SEEDS), SYSROOT)
    amd = deb_packages("amd64")   # wayland-scanner 1.18 and its libraries, run on this machine
    # this machine's C library runs it (Debian's beside this machine's other libraries would not load)
    deb_install(amd, [n for n in deb_closure(amd, ["libwayland-bin"]) if n != "libc6"], SYSROOT / "host")
    for p in SYSROOT.rglob("*"):   # absolute links point into this machine: made relative, as unpack_sniper does
        if p.is_symlink() and os.readlink(p).startswith("/"):
            base = SYSROOT / "host" if (SYSROOT / "host") in p.parents else SYSROOT
            target = base / os.readlink(p).lstrip("/")
            p.unlink()
            p.symlink_to(os.path.relpath(target, p.parent))
    inc = SYSROOT / "usr/local/include"
    inc.mkdir(parents=True, exist_ok=True)
    tmp = DL / "vulkan-headers"
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir()
    with tarfile.open(fetch(*PINS["vulkan-headers"])) as t:
        t.extractall(tmp, filter="tar")
    (top,) = tmp.iterdir()
    for d in ("vulkan", "vk_video"):
        shutil.copytree(top / "include" / d, inc / d, dirs_exist_ok=True)
    shutil.rmtree(tmp)
    vulkan_stub()


def vulkan_stub() -> None:
    """A link-time libvulkan.so.1 exporting every function vulkan_core.h declares: Debian 11's loader (1.2.162) lacks
    the 1.3 core the runtime calls. Never shipped: the player's own loader (release.py SYSTEM_LIBS) runs the game."""
    import re

    names = sorted(set(re.findall(r"VKAPI_ATTR \S+ VKAPI_CALL (vk\w+)\(",
                                  (SYSROOT / "usr/local/include/vulkan/vulkan_core.h").read_text())))
    lib = SYSROOT / "usr/local/lib"
    lib.mkdir(parents=True, exist_ok=True)
    src = DL / "vulkan_stub.c"
    src.write_text("".join(f"void {n}(void) {{}}\n" for n in names))
    subprocess.run([*cc().split(), "-shared", "-nostdlib", "-fPIC", "-Wl,-soname,libvulkan.so.1", "-o",
                    str(lib / "libvulkan.so"), str(src)], check=True)  # fmt: skip
    print(f"vulkan stub: {len(names)} functions")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--arch", choices=tuple(ARCHES), default="x86_64")
    args = ap.parse_args()
    use_arch(args.arch)
    DL.mkdir(parents=True, exist_ok=True)
    if not STAMP.is_file():
        unpack_sniper() if args.arch == "x86_64" else unpack_bullseye()
        add_deps()
        build_sdl3()
        STAMP.write_text("")
    print(SYSROOT)


if __name__ == "__main__":
    main()
