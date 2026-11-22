"""Merge enabled mods that replace the same game archive, like switch2-mod-merger does in the browser.

Why: several Switch mods ship a full copy of one archive (general_format...sarc.zs) with a few inner
files changed. The overlay serves one file per path, so enabling two such mods would silently drop one.
This merges them instead: inner files changed by one mod are taken, files changed by several take the
winner from choices.json (or vanilla, from the loose RomFS), defaulting to the last-enabled mod. The
merged archive lands in <game>/mods/.merged/romfs, which the game reads as one more overlay, last.

Usage: merge.py --game EXEDIR --romfs ROMFSDIR --enable A,B [--choices PATH] [--merged DIR]
  --enable: enabled mods in load order (later wins); empty: nothing to merge, the .merged dir is cleared
  --choices: JSON [{"archive", "inner", "winner", "mods"}] (the Mods page's picks); no entry for an
             inner file means the default, the last-enabled mod
  Prints "merged <archive> <inners> auto=<n> conflicts=<n>", one "conflict <archive> <inner> <mods> <winner>"
  per contested inner file, "warn <text>", or "error <code> <detail>"; exit 0 done (or fresh), 1 failed.
  Writes .merged/conflicts.json for the Mods page's picker and .merged/state.json to skip fresh runs.
"""

import argparse
import compression.zstd as zstd
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "prepare"))
from nup import romfs_read  # noqa: E402  (a loose RomFS or a .nup package)


def game_file(romfs, rel):
    """The game's own file at rel (a loose RomFS or a .nup), None when it has none."""
    try:
        return romfs_read(Path(romfs), rel)
    except (KeyError, OSError):
        return None

HASH_KEY, DATA_ALIGN = 0x65, 0x100
MERGE_CACHE_VERSION = 3  # the conflict picker needs to know whether each inner file has an original


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def filename_hash(name):
    h = 0
    for ch in name.encode("utf-8"):
        h = ((h * HASH_KEY) + ch) & 0xFFFFFFFF
    return h


def parse_sarc_ordered(raw):
    """(files in data-section order, little-endian, version)."""
    if raw[:4] != b"SARC":
        raise ValueError("not a SARC archive")
    bom = struct.unpack_from("<H", raw, 6)[0]
    if bom != 0xFEFF:
        raise ValueError("big-endian SARC not supported")
    hlen, _, fsize, data_off, ver, _ = struct.unpack_from("<HHIIHH", raw, 4)
    pos = hlen + 12
    if raw[hlen:hlen + 4] != b"SFAT":
        raise ValueError("no SFAT")
    nfiles = struct.unpack_from("<H", raw, hlen + 6)[0]
    entries = []
    for _ in range(nfiles):
        h, field, start, end = struct.unpack_from("<IIII", raw, pos)
        entries.append((h, (field & 0xFFFFFF) * 4, start, end))
        pos += 16
    strtab = pos + 8
    out = []
    for h, noff, start, end in entries:
        e = raw.index(b"\0", strtab + noff)
        name = raw[strtab + noff:e].decode("utf-8")
        if filename_hash(name) != h:
            raise ValueError(f"hash mismatch for {name}")
        out.append((start, name, raw[data_off + start:data_off + end]))
    out.sort()
    return [(n, d) for _, n, d in out], ver


def build_sarc(files, version=0x100):
    """files: [(name, data)] in data-section order. Lies out like switch2-mod-merger's buildSarc."""
    seen, ordered = set(), []
    for name, _ in files:
        if name not in seen:
            seen.add(name)
            ordered.append(name)
    blob = {n: d for n, d in files}
    files = [(n, blob[n]) for n in ordered]
    nb = [n.encode("utf-8") for n, _ in files]
    noff, tsize = [], 0
    for b in nb:
        noff.append(tsize)
        tsize = align_up(tsize + len(b) + 1, 4)
    starts, dsize = [], 0
    for _, d in files:
        s = align_up(dsize, DATA_ALIGN)
        starts.append(s)
        dsize = s + len(d)
    doff = 0x14 + 0x0C + len(files) * 0x10 + 0x08 + tsize
    total = doff + dsize
    out = bytearray(total)
    struct.pack_into("<HHIIHH", out, 4, 0x14, 0xFEFF, total, doff, version, 0)
    out[0:4] = b"SARC"
    pos = 0x14
    out[pos:pos + 4] = b"SFAT"
    struct.pack_into("<HHI", out, pos + 4, 0x0C, len(files), HASH_KEY)
    pos += 0x0C
    nodes = sorted(((filename_hash(n), (0x01 << 24) | (noff[i] // 4), starts[i],
                     starts[i] + len(d)) for i, (n, d) in enumerate(files)))
    for h, field, s, e in nodes:
        struct.pack_into("<IIII", out, pos, h, field, s, e)
        pos += 0x10
    out[pos:pos + 4] = b"SFNT"
    struct.pack_into("<HH", out, pos + 4, 0x08, 0)
    pos += 0x08
    for i, b in enumerate(nb):
        out[pos + noff[i]:pos + noff[i] + len(b)] = b
    pos += tsize
    for i, (_, d) in enumerate(files):
        out[pos + starts[i]:pos + starts[i] + len(d)] = d
    return bytes(out)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fail(code, detail):
    print(f"error {code} {detail}", flush=True)
    return 1


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")  # mods.cpp reads this pipe as UTF-8
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", required=True)
    ap.add_argument("--romfs", required=True)
    ap.add_argument("--enable", default="")
    ap.add_argument("--choices", default="",
                    help="picked winners; kept outside --merged, which this wipes")
    ap.add_argument("--merged", default="")
    args = ap.parse_args()
    mods_dir = os.path.join(args.game, "mods")
    merged = args.merged or os.path.join(mods_dir, ".merged")
    mromfs = os.path.join(merged, "romfs")
    enabled = [m for m in args.enable.split(",") if m]
    if not os.path.exists(args.romfs):
        return fail("romfs", f"{args.romfs} is not the game's RomFS (a folder or a .nup)")
    picks = []
    if args.choices and os.path.isfile(args.choices):
        try:
            with open(args.choices, encoding="utf-8") as f:
                picks = json.load(f)
            if not isinstance(picks, list):
                raise ValueError("not a list")
        except ValueError:
            return fail("io", "choices.json is not JSON")
    choices = {(p.get("archive"), p.get("inner")): p for p in picks if isinstance(p, dict)}
    # An unchanged mod list can still need a rebuild when the player switches the linked game files.
    romfs_source = os.path.normcase(os.path.realpath(args.romfs))
    # freshness: same source, same mods, same choices, nothing newer than the outputs
    state_path = os.path.join(merged, "state.json")
    manifest_path = os.path.join(mromfs, "manifest.tsv")
    current = {"version": MERGE_CACHE_VERSION, "mods": enabled, "choices": picks, "romfs": romfs_source}
    try:
        fresh = os.path.isfile(manifest_path)
        fresh = fresh and json.load(open(state_path, encoding="utf-8")) == current
        if fresh:
            outs = [os.path.join(r, f) for r, _, fs in os.walk(merged) for f in fs]
            newest_out = max(os.path.getmtime(p) for p in outs)
            for m in enabled:
                d = os.path.join(mods_dir, m)
                for r, _, fs in os.walk(d):
                    if ".merged" in r:
                        continue
                    for f in fs:
                        if os.path.getmtime(os.path.join(r, f)) > newest_out:
                            fresh = False
                            break
        if fresh:
            print("fresh", flush=True)
            return 0
    except (OSError, ValueError):
        pass
    import shutil
    shutil.rmtree(merged, ignore_errors=True)
    os.makedirs(mromfs, exist_ok=True)
    if not enabled:
        json.dump(current, open(state_path, "w"), indent=2)
        print("done empty", flush=True)
        return 0
    # enabled mods' archives, in load order
    per_archive = {}   # rel -> [(mod, inner map, version)]
    for m in enabled:
        rdir = os.path.join(mods_dir, m, "romfs")
        if not os.path.isdir(rdir):
            continue
        for root, _, files in os.walk(rdir):
            if "manifest.tsv" in files:
                files = [f for f in files if f != "manifest.tsv"]
            for fn in sorted(files):
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, rdir).replace(os.sep, "/")
                if not (rel.endswith(".sarc.zs") or rel.endswith(".sarc") or rel.endswith(".msarc")):
                    continue
                with open(full, "rb") as f:
                    raw = f.read()
                if rel.endswith(".zs"):
                    try:
                        raw = zstd.decompress(raw)
                    except Exception as e:
                        return fail("io", f"{m}/{rel}: zstd: {e}")
                try:
                    inners, ver = parse_sarc_ordered(raw)
                except (ValueError, struct.error):   # struct.error: a header cut short, which no one caught
                    continue   # not an archive (a texture etc.): the overlay serves it whole
                per_archive.setdefault(rel, []).append((m, dict(inners), ver))
    conflicts, merged_n, auto_n = [], 0, 0
    manifest = []
    for rel, versions in sorted(per_archive.items()):
        if len(versions) < 2:
            continue
        vanilla, verr = None, None
        lraw = game_file(args.romfs, rel)
        if lraw is not None:
            if rel.endswith(".zs"):
                try:
                    lraw = zstd.decompress(lraw)
                except Exception:
                    lraw = None
            if lraw is not None:
                try:
                    vanilla, verr = parse_sarc_ordered(lraw)
                    vanilla = dict(vanilla)
                except (ValueError, struct.error):
                    vanilla = None
        # inner file -> {mod: data} for mods that differ from vanilla (or add)
        changed = {}
        for m, inners, _ in versions:
            for name, data in inners.items():
                v = vanilla.get(name) if vanilla is not None else None
                if v is None or v != data:
                    changed.setdefault(name, {})[m] = data
        base = dict(vanilla) if vanilla is not None else {}
        order = list(base)
        winners = {}
        for name, by_mod in changed.items():
            if name not in base:
                order.append(name)
            distinct = set(by_mod.values())
            if len(by_mod) == 1 or len(distinct) == 1:
                winners[name] = next(iter(by_mod.values()))
                auto_n += 1
            else:
                pick = choices.get((rel, name))
                want = pick.get("winner") if pick else None
                mods_here = [m for m, _, _ in versions if m in by_mod]
                has_original = name in base
                if want == "vanilla" and has_original:
                    winners[name] = base[name]
                elif want != "vanilla" and want in mods_here:
                    winners[name] = by_mod[want]
                else:
                    # A stored Original pick can outlive the original file.
                    pick = None
                    want = mods_here[-1]
                    winners[name] = by_mod[want]
                resolved = pick is not None and set(pick.get("mods") or mods_here) == set(mods_here)
                conflicts.append((rel, name, mods_here, want, resolved, has_original))
        if not changed and all(vanilla is not None for _, _, v in versions):
            continue   # identical content: nothing to merge
        out = [(n, winners[n] if n in winners else base[n]) for n in order]
        ver = versions[0][2]
        arc = build_sarc(out, ver)
        dest = os.path.join(mromfs, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        if rel.endswith(".zs"):
            arc = zstd.compress(arc, level=4)
        with open(dest, "wb") as f:
            f.write(arc)
        merged_n += 1
        relq = rel
        manifest.append((relq, dest))
    # manifest for the merged dir (same format the overlay loader reads)
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as mf:
        for relq, dest in sorted(manifest):
            size = os.path.getsize(dest)
            orig = game_file(args.romfs, relq)
            osha = hashlib.sha256(orig).hexdigest() if orig is not None else "-"
            mf.write(f"{relq}\t{size}\t{sha256_file(dest)}\t{osha}\n")
    with open(os.path.join(merged, "conflicts.json"), "w", encoding="utf-8") as f:
        json.dump([{"archive": a, "inner": i, "mods": m, "winner": w, "resolved": r, "has_original": original}
                   for a, i, m, w, r, original in conflicts], f, indent=2)
    with open(state_path, "w", encoding="utf-8") as f:
        json.dump(current, f, indent=2)
    for a, i, m, w, _, _ in conflicts:
        try:
            print(f"conflict {a} {i} {','.join(m)} {w}", flush=True)
        except BrokenPipeError:
            os._exit(0)   # the files are written; the menu went away mid-print
    try:
        print(f"done merged={merged_n} auto={auto_n} conflicts={len(conflicts)}", flush=True)
    except BrokenPipeError:
        os._exit(0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
