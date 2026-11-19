# i18n/ui

The texts of the in-game menu (F1), the start screen, the setup page and the performance overlay, as gettext files.
The game's own texts are not here: the game ships them in 16 languages and picks one with `lang` in `mkw.ini`.

| File | Contents |
|---|---|
| `mkw.pot` | The English: each entry's `msgctxt` is its id in `runtime/gfx/nvn/host/ui_text.h`, its `msgid` the English text |
| `ja.po`, `fr.po`, `de.po`, `it.po`, `es.po`, `nl.po`, `pt.po`, `pt-BR.po`, `ru.po`, `zh-Hans.po`, `zh-Hant.po`, `ko.po` | One translation per language the game ships |
| `es-419.po`, `ca.po`, `gl.po`, `eu.po`, `pl.po`, `tr.po`, `uk.po`, `th.po` | Menu languages the game's first version does not ship: its texts are then in Spanish (ca, gl, eu) or English (uk), and in our own translations of the game (ca, gl, eu, tr: `i18n/game`); `pl` and `th` are in the game itself since its updates 1.5.0 and 1.7.0 (a 1.8.0 dump) |

The first translation of each language is ours, with the game's own words for its modes and items
(`tools/i18n/game_texts.py`); corrections by native speakers are welcome.

`tools/build/build.py` compiles them into the runtime (`tools/i18n/catalog.py`). An empty or fuzzy entry shows the
English. To fix a translation, edit its `msgstr` (Poedit or any text editor) and open a pull request; keep every
`%s`, `%d`, `%.1f`... in the same order (`tests/runtime/test_ui_text.py` checks it). A new or changed English text goes into
`mkw.pot` and then `uv run tools/i18n/catalog.py update`.
