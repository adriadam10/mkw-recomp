# i18n/game

The game's own texts in languages it does not ship, over one it does: `ca`, `gl` and `eu` over its Spain Spanish.
Each `base/<lang>.tsv` has one `label<TAB>text` line per translated text; labels are the game's (`MsgMain/Menu/2203`), and
only our translation is here, never Nintendo's text. A label left out stays in Spanish.

The game's updates add labels (they go in `base/<lang>.tsv` too: a copy without them ignores them) and reword or retag a few
of the old ones: those go in `<version>/<lang>.tsv` (`1.8.0/ca.tsv`), used over `base/<lang>.tsv` for a copy of
that version or later, so a 1.0.0 copy keeps its own translation of them.

`tools/i18n/game_texts.py` lists what is left (`todo ca`, from your own copy of the game, with its control tags such
as `{0E ...}` to keep as they are) and builds the files the game reads (`build ca`); the setup builds them for the
player into `lang/<lang>/`, and choosing the language in Settings > Game shows them.

| Folder | Contents |
|---|---|
| [base/](base/README.md) | One `<lang>.tsv` per language: Catalan, Galician, Basque |
| `1.8.0/` | The texts 1.8.0 changed, per language |
