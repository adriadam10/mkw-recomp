# tools/i18n

| File | What | Usage |
|---|---|---|
| `game_texts.py` | The game's own texts of one `romfs/Mals` folder (SARC of MSBT) as label and text lines: the vocabulary a translation reuses | `uv run tools/i18n/game_texts.py EUde [-o DIR]` |
| `game_fonts.py` | The game's fonts with the letters a translation needs and they lack (Turkish ğ Ğ ş Ş İ), composed from their own breve, dot and cedilla into the translation's RomFS overlay; `game_texts.py build` and the setup call it | `uv run tools/i18n/game_fonts.py missing\|build tr` |
| `catalog.py` | The menu's text catalogs (`i18n/ui/mkw.pot`, `i18n/ui/<lang>.po`): merges the English template into every translation (changed English marked fuzzy), shows how far each language is, and generates the C arrays `build.py` compiles | `uv run tools/i18n/catalog.py update\|status\|gen DIR` |
| `fill.py` | Writes new or changed menu texts in every language from a JSON table `{msgctxt: {lang: text}}`: only empty or fuzzy entries (a fuzzy one loses its flag), never a checked translation | `uv run tools/i18n/fill.py TABLE.json` after `catalog.py update` |
| `fill_mod_strings.py` | One-shot fill of the Mods page and last-run notice strings in all catalog languages (author-provided, translators refine): only empty entries, never a translator's words | run once per new string batch, then `catalog.py status` to confirm |
