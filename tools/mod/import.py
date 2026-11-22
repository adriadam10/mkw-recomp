"""Import a Switch-style mod into <game>/mods for the start screen's Mods page.

Why: mods ship in the emulators' layout (romfs/ and/or exefs/ with <build id>.ips + .pchtxt inside), as
GameBanana has them: zip, rar or 7z, with those folders at the root, under one folder, deeper, or under
atmosphere/contents/<title id>/. The port reads a mod from <game>/mods/<name>/ with a manifest.tsv over its romfs/
(runtime/sdk/fs/romfs.c) and a mod.json the Mods page shows. This copies the mod in as it comes, checks the exefs
patches name this game's build id, and writes both files. A mod's plugin/ (native libraries, runtime/plugin) is copied
as it is: the runtime loads it when the mod is on.

Usage: import.py --game EXEDIR --romfs ROMFS [--exefs DIR] [--build-id HEX] (--zip FILE | --folder DIR)
                 [--name NAME] [--variant PATH]
  --game: the folder of mkw / mkw.exe (mods/ is made inside it)
  --romfs: the game's RomFS the manifest's original hashes come from: a loose folder or a .nup package
  --zip: a mod archive, .zip, .rar or .7z (rar and 7z through tar, which is bsdtar on Windows 10 and later)
  --folder: an extracted mod folder
  --variant: when the archive holds several mods (options), the one to take, as "error variants" listed it
  Prints "done <name>" with the mod's file counts, or "error <code> <detail>"; exit 0 done, 1 failed,
  2 bad command line. Codes: layout (no romfs/, exefs/ or plugin/ inside), variants (several; the detail lists them,
  tab-separated), name (nothing usable), io (a copy or hash failed), big (over the unpacking limits),
  unpack (a rar or 7z with no tar that reads it). An exefs .ips that names another build id is kept and
  listed as incompatible.
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import subprocess
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "prepare"))
from nup import romfs_read  # noqa: E402  (a loose RomFS or a .nup package)

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ips  # noqa: E402  (a mod's .ips / .pchtxt as patches of the game's code)

# An archive that unpacks to more is refused (a zip bomb, or not a mod): the largest MKW mods are a few hundred MB
MAX_BYTES, MAX_FILES = 4 << 30, 50_000

BUILD_ID_OFF, BUILD_ID_LEN = 0x40, 8


def fail(code, detail):
    print(f"error {code} {detail}", flush=True)
    return 1


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def game_build_id(exefs_main):
    with open(exefs_main, "rb") as f:
        f.seek(BUILD_ID_OFF)
        return f.read(BUILD_ID_LEN).hex()


def tar_tool():
    """bsdtar (libarchive), which reads rar and 7z: Windows ships it as System32/tar.exe, Linux distributions as bsdtar.
    Not the first "tar" on PATH on Windows: Git's or MSYS2's usr/bin there is GNU tar, which takes "C:\\..." for a
    remote host ("Cannot connect to C: resolve failed") and reads no rar or 7z."""
    if found := shutil.which("bsdtar"):
        return found
    system = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "tar.exe"
    if os.name == "nt" and system.is_file():
        return str(system)
    return shutil.which("tar")


def unpack(archive, dst):
    """The archive into dst, refusing absolute or .. names and anything past MAX_BYTES / MAX_FILES. Returns an error
    (code, detail) or None."""
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as z:
            infos = [i for i in z.infolist() if not i.is_dir()]
            if len(infos) > MAX_FILES or sum(i.file_size for i in infos) > MAX_BYTES:
                return "big", f"{len(infos)} files, {sum(i.file_size for i in infos) >> 20} MiB"
            for i in infos:
                if os.path.isabs(i.filename) or ".." in Path(i.filename).parts:
                    return "layout", f"unsafe name {i.filename}"
            z.extractall(dst)
        return None
    tar = tar_tool()
    if not tar:
        return "unpack", "no tar to read this archive"
    listing = subprocess.run([tar, "-tf", archive], capture_output=True, text=True, errors="replace")
    if listing.returncode:
        return "unpack", (listing.stderr.strip().splitlines() or ["not an archive tar reads"])[-1]
    names = [n for n in listing.stdout.splitlines() if n and not n.endswith("/")]
    if len(names) > MAX_FILES:
        return "big", f"{len(names)} files"
    if any(os.path.isabs(n) or ".." in Path(n).parts for n in names):
        return "layout", "unsafe names"
    if subprocess.run([tar, "-xf", archive, "-C", dst], capture_output=True).returncode:
        return "unpack", "tar could not unpack it"
    if any(p.is_symlink() for p in Path(dst).rglob("*")):
        return "layout", "the archive holds links"
    if sum(p.stat().st_size for p in Path(dst).rglob("*") if p.is_file() and not p.is_symlink()) > MAX_BYTES:
        return "big", "over the size limit"
    return None


def mod_roots(top):
    """The folders under top that hold romfs/, exefs/ or plugin/ (any letter case), shallowest first; macOS' __MACOSX
    and dot folders skipped. One per mod: an archive with options (several romfs/) gives several."""
    roots = []
    for here, dirs, _ in os.walk(top):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d != "__MACOSX")
        if {d.lower() for d in dirs} & set(PARTS):
            roots.append(here)
            dirs[:] = []   # a mod's own romfs/ may hold folders named like these: do not look inside
    return roots


# a mod's parts: romfs/ overlays the game's files, exefs/ patches its code, plugin/ holds native libraries the runtime
# loads with the mod on (runtime/plugin/mkw_plugin.h)
PARTS = ("romfs", "exefs", "plugin")


def part_dir(root, part):
    """root's romfs/, exefs/ or plugin/ whatever its letter case, or None."""
    for d in os.listdir(root):
        if d.lower() == part and os.path.isdir(os.path.join(root, d)):
            return os.path.join(root, d)
    return None


def original_sha(romfs, rel):
    """SHA-256 of the game's own file at rel (a loose RomFS or a .nup), "-" when the game has none."""
    try:
        return hashlib.sha256(romfs_read(Path(romfs), rel)).hexdigest()
    except (KeyError, OSError):
        return "-"


def clean_name(name):
    keep = "".join(c if c.isalnum() or c in ("-", "_", " ") else "_" for c in name).strip()
    return keep.strip("._")[:64]


def main():
    # SDL reads these pipes as UTF-8; Windows Python otherwise uses the active code page, even with -I.
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8", errors=sys.stderr.errors)
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", required=True)
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--zip", dest="zip_path")
    ap.add_argument("--folder", dest="folder")
    ap.add_argument("--name", default="")
    ap.add_argument("--variant", default="", help="the mod to take from an archive with several (as listed)")
    ap.add_argument("--exefs", default="",
                    help="the game's exefs folder (default: <game>/exefs); its main names the build id")
    ap.add_argument("--build-id", default="",
                    help="16 hex chars of exefs/main's build id, instead of reading it from the exefs")
    args = ap.parse_args()
    if bool(args.zip_path) == bool(args.folder):
        print("error usage: one of --zip or --folder", flush=True)
        return 2
    mods_dir = os.path.join(args.game, "mods")
    loose = args.romfs
    if not os.path.exists(loose):
        return fail("romfs", f"{loose} is not the game's RomFS (a folder or a .nup)")
    try:
        exefs = args.exefs or os.path.join(args.game, "exefs")
        game_id = args.build_id.lower() if args.build_id else game_build_id(os.path.join(exefs, "main"))
    except OSError as e:
        return fail("nogame", f"{e.filename or args.game}")
    if len(game_id) != 16 or any(c not in "0123456789abcdef" for c in game_id):
        return fail("usage", "bad --build-id")

    with tempfile.TemporaryDirectory(prefix="mkwmod") as tmp:
        if args.zip_path:
            try:
                err = unpack(args.zip_path, tmp)
            except (zipfile.BadZipFile, OSError) as e:
                return fail("io", str(e))
            if err:
                return fail(*err)
            top, stem = tmp, os.path.splitext(os.path.basename(args.zip_path))[0]
        else:
            if not os.path.isdir(args.folder):
                return fail("layout", "not a folder")
            top, stem = args.folder, os.path.basename(os.path.normpath(args.folder))
        roots = mod_roots(top)
        if args.variant:
            roots = [r for r in roots if os.path.relpath(r, top).replace(os.sep, "/") == args.variant]
        if not roots:
            return fail("layout", "no romfs/, exefs/ or plugin/ inside")
        if len(roots) > 1:
            return fail("variants", "\t".join(os.path.relpath(r, top).replace(os.sep, "/") for r in roots))
        src = roots[0]
        # the mod's name: its folder, unless that is the archive's root, atmosphere's title id folder or "contents"
        rel = os.path.relpath(src, top).replace(os.sep, "/")
        last = rel.split("/")[-1]
        default = stem if rel == "." or last.lower() in ("contents", "0400c3f00006e000") else last
        if any(part_dir(src, p) and not os.path.isdir(os.path.join(src, p)) for p in PARTS):
            # RomFS/ExeFS/Plugin spelled otherwise: copy under the names the port reads
            fixed = os.path.join(tmp, ".case")
            shutil.copytree(src, fixed, symlinks=False)
            for part in PARTS:
                d = part_dir(fixed, part)
                if d and os.path.basename(d) != part:
                    os.rename(d, os.path.join(fixed, part))
            src = fixed
        has_romfs = os.path.isdir(os.path.join(src, "romfs"))
        has_exefs = os.path.isdir(os.path.join(src, "exefs"))
        has_plugin = os.path.isdir(os.path.join(src, "plugin"))

        name = clean_name(args.name) if args.name else clean_name(default)
        if not name:
            return fail("name", "empty after cleaning")
        shipped_patches = {}
        try:
            with open(os.path.join(src, "mod.json"), encoding="utf-8") as _m:
                shipped_patches = json.load(_m).get("patches", {}) or {}
        except (OSError, ValueError):
            pass
        usable = [p for p in shipped_patches.get("spans", []) + shipped_patches.get("sites", []) if isinstance(p, dict)]
        image = []
        if has_exefs and not usable:
            # as GameBanana has them: the .ips applied to the code before the generator lifts it (enabling or
            # removing the mod then builds the game code again: tools/prepare/prepare.py)
            files = [p for p in Path(src, "exefs").iterdir() if p.suffix.lower() in (".ips", ".pchtxt")]
            try:
                image = ips.image_patches(Path(exefs), files)
            except (ips.PatchFailed, OSError) as e:
                return fail("notported", f"{default}: {e}")
        dest = os.path.join(mods_dir, name)
        if os.path.lexists(dest):
            return fail("name", f"{name} already imported")
        shutil.copytree(src, dest, symlinks=False)

        builds, matching = [], False
        if has_exefs:
            for fn in sorted(os.listdir(os.path.join(dest, "exefs"))):
                if fn.endswith(".ips"):
                    bid = fn[:-4].lower()
                    builds.append(bid)
                    # the file names the full build id, the game its first 8 bytes (recomp/versions.py)
                    matching = matching or bid.startswith(game_id) or game_id.startswith(bid)
        n_romfs, warnings = 0, set()
        if has_romfs:
            lines = []
            for root, _, files in os.walk(os.path.join(dest, "romfs")):
                for fn in sorted(files):
                    full = os.path.join(root, fn)
                    rel = os.path.relpath(full, os.path.join(dest, "romfs")).replace(os.sep, "/")
                    if rel == "manifest.tsv":
                        continue
                    # files the port treats its own way: the page warns, the mod still installs
                    if rel.startswith("Shader/"):
                        warnings.add("shaders")   # translated at setup: a mod's shaders need the setup again
                    elif rel.startswith("System/GymlArchive/general_format"):
                        warnings.add("graphics")   # the game's graphics settings, which the port's own override
                    size = os.path.getsize(full)
                    osha = original_sha(loose, rel)
                    lines.append(f"{rel}\t{size}\t{sha256_file(full)}\t{osha}\n")
                    n_romfs += 1
            with open(os.path.join(dest, "romfs", "manifest.tsv"), "w", encoding="utf-8",
                       newline="\n") as m:
                m.writelines(lines)
        # a shipped mod declares its port in mod.json (title, switches, patches); import keeps them
        shipped, switches = {}, {}
        try:
            with open(os.path.join(src, "mod.json"), encoding="utf-8") as m:
                shipped = json.load(m)
                switches = shipped.get("switches", {}) or {}
        except (OSError, ValueError):
            pass
        # the name the player typed (or a renamed folder) is what the row shows; the shipped title only
        # stands in when the mod keeps its own folder name
        title = name if (args.name or default != name) else (shipped.get("title") or name)
        meta = {"name": name, "title": title, "has_romfs": has_romfs,
                "romfs_files": n_romfs, "has_exefs": has_exefs, "has_plugin": has_plugin, "exefs_builds": builds,
                "exefs_match": matching if has_exefs else True, "switches": switches,
                "warnings": sorted(warnings)}
        if shipped.get("patches"):
            meta["patches"] = shipped["patches"]
        if image:
            meta["image"] = image
        with open(os.path.join(dest, "mod.json"), "w", encoding="utf-8", newline="\n") as m:
            json.dump(meta, m, indent=2)
            m.write("\n")
    try:
        print(f"done {name} romfs={n_romfs} exefs={','.join(builds) if builds else '-'}", flush=True)
    except BrokenPipeError:
        os._exit(0)   # the mod is written; the menu went away mid-print
    return 0


if __name__ == "__main__":
    sys.exit(main())
