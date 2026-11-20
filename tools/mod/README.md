# tools/mod

Mod import and merging for the start screen's Mods page (`runtime/ui/launcher/mods.cpp` runs these
tools the way `launcher_setup.cpp` runs `tools/prepare/prepare.py`: the package's Python, one event
per line). The importer emits UTF-8 on its stdout/stderr pipes on every platform, so Unicode names and paths
reach the launcher intact.

| File | What it is |
|---|---|
| `gamebanana.py` | GameBanana's Mario Kart World mods for the Mods page's second tab: list (by downloads, newest, likes), search, a mod's page (author, license, content rating, description, files) and the download of one of its files, which then goes through `import.py`. Public apiv11, our User-Agent, answers cached 10 minutes, at most 60 requests a minute. Rows carry the content rating's visibility (show, warn, hide) so the page blurs rated mods unless its NSFW-mods choice is Show. Downloads only from `gamebanana.com`, only files its virus scan calls clean, MD5 checked. Nothing is hosted by this project |
| `ips.py` | A mod's exefs patches (`.ips`/IPS32 or `.pchtxt`, named by the patched module's build id) as bytes of the game's own code (`image_patches`), which `import.py` keeps in `mod.json` `"image"`: applied to the module before the generator lifts it, so code caves, branches and new functions all go; a write outside `.text` or another build's id is refused with where. Also `convert`, the same as `sites`/`spans` when a patch fits them (switchable without a rebuild, rare in GameBanana's mods) |
| `import.py` | Copies a mod as GameBanana has it (`.zip`, `.rar` or `.7z` through `bsdtar`, or a folder; `romfs/` and/or `exefs/` at any depth, also `atmosphere/contents/<title id>/`; an archive with several mods lists them for the page to pick, `--variant`) into `<game>/mods/<name>/`, with unpacking limits (4 GB, 50000 files), against a loose RomFS or a `.nup`, writes the `romfs/manifest.tsv` the overlay loader reads and a `mod.json` the Mods page shows. A shipped mod carries its port in `mod.json` (`switches` with bare names, e.g. `{"CLASSICGP_MAPLINES": "1"}`): enabling the mod sets them, like the patch sets the port bakes in. Import keeps the shipped `title` and `switches` and recomputes the rest |
| `merge.py` | Merges the enabled mods' shared game archives (`<mod>/romfs/**/*.sarc.zs`): inner files changed by one mod are taken, files changed by several take the winner from `.merged/choices.json` (the Mods page's picker; `vanilla` is the loose RomFS) defaulting to the last-enabled mod, like [switch2-mod-merger](https://github.com/SavageCore/switch2-mod-merger) does in the browser, which this mirrors. Writes the merged tree to `<game>/mods/.merged/romfs` plus its `manifest.tsv`, `conflicts.json` for the picker and `state.json` to skip fresh runs. Code patches are never merged: two mods patching one address stop the generate with both names |
| `mod.json` `patches` | A mod's code patches, merged into the generated code by `gen.py --mods`: `sites` (one instruction: `set`, `env`, `module`, hex `off`/`orig`/`repl`) and `spans` (a skipped branch target: `set`, `env`, `module`, hex `off`/`words...`; all original words, verified at generate time). Two mods patching one address fail loudly instead of merging |

## The `mod.json` spec

A mod is a folder with the Switch layout and a `mod.json` beside it:

```
Timer/
├── mod.json
├── romfs/            optional: overlays the game's RomFS
│   └── Layout/...    (import.py writes romfs/manifest.tsv itself)
├── exefs/            optional: the original Switch patches, for Switch/emulator use
│   ├── <build id>.ips
│   └── <build id>.pchtxt
└── plugin/           optional: native libraries (.so, .dll) the runtime loads with the mod on
                      (runtime/plugin/mkw_plugin.h; mod.json "has_plugin", and the page warns)
```

```json
{
  "name": "Timer",
  "title": "Timer",
  "has_romfs": true,
  "romfs_files": 1,
  "has_exefs": true,
  "exefs_builds": [
    "d00e4f399e411d0613aa8d26b1c02fef00000000"
  ],
  "exefs_match": true,
  "switches": {
    "TIMER": "1"
  },
  "patches": {
    "spans": [
      {
        "set": "timer",
        "env": "MKW_TIMER",
        "module": "main",
        "off": "48e728",
        "words": [
          "52800028",
          "39004328",
          "... 22 words in all ..."
        ]
      }
    ],
    "sites": [
      {
        "set": "classicgp",
        "env": "MKW_CLASSICGP",
        "module": "main",
        "off": "7d201c",
        "orig": "51001D4A",
        "repl": [
          "5100214A"
        ]
      }
    ]
  }
}
```

| Field | What it is |
|---|---|
| `name` | the folder name; what `MODS` lists and the menu toggles |
| `title` | what the row shows (a rename sets it) |
| `has_romfs`, `romfs_files`, `has_exefs`, `exefs_builds`, `exefs_match` | written by `import.py` from what it found: shown in the row, and `exefs_match` is false when the shipped `.ips` names another game build |
| `switches` | bare switch names, **without** `MKW_`: enabling the mod sets `MKW_<name>=<value>` for the next start. Every switch a patch set uses must appear here, or enabling the mod would not turn its code on |
| `patches.sites` | one instruction replaced: `set` (the patch set's name), `env` (its `MKW_` switch), `module`, `off` (hex offset inside that module), `orig` (the word the binary must hold), `repl` (the words to run instead, same count as `orig` - one) |
| `patches.spans` | a region the switch skips whole, for a mod whose patch is a branch over code (a site cannot be a branch): `set`, `env`, `module`, `off`, `words` (every original word, in order). With the switch on, the region is jumped over; with it off, it runs as shipped |

Rules the generate enforces (`recomp/patches.py`, `recomp/gen.py`):

- `off` is the offset in the module's image, the same space the recomp's own `recomp/patches-mkw*.txt` use. Switch `.pchtxt` files often declare `@flag offset_shift 0x100`: subtract `0x100` from those addresses.
- `orig` and every span word must match the game files; a mismatch stops the generate with the offset and both words, so a mod for another build cannot be applied.
- `repl` may not contain a branch: the replacement keeps the function's control flow. A patch that needs a branch is a span.
- Two mods patching one address, or a site inside a span, stop the generate naming both sides - they are never merged silently.

`gen.py --mods DIR` (default: the `mods/` beside `--exefs`) merges every mod's patches into the generated table, so the game's own tables stay the game's own. An exefs mod without patches is refused at import rather than imported and then doing nothing.

Porting an exefs mod is reading its `.pchtxt` and turning each entry into a `site` (or a `span`), then checking the words against the game build the port targets. `tools/analisis/bin/disasm.py` and `q.py` give the surrounding code when an entry needs rewriting into an equivalent that is not a branch.
