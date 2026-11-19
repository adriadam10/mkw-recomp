# tools/plugin

The native plugin API's test and example (`runtime/plugin/`).

| File | What it is |
|---|---|
| `probe.sh` | Builds `probe_plugin.c` as the mod `plugin-probe` (`build/mods/plugin-probe/plugin/`) and runs `build/mkw --probe-plugin` with it on: a call through main's PLT reaches the plugin's override by name, an indirect call to the game's definition its override by address |
| `example.sh` | Builds `example_plugin.c` into an importable mod, as a folder (`build/mods-import/example-plugin/`) and as its `.zip`, the two shapes the launcher's Mods page imports (Linux `.so`) |
| `example_plugin.c` | A plugin that changes nothing in the game: a notice when the game code is loaded and a box with the versions in a corner (`notice`, `overlay_set`, `overlay_show`), and an options page (`mkw_mod_page`) with every `MkwPage` widget, in English and Brazilian Portuguese: the box on or off, its corner, the notice's text and a list of names to add and remove, read back in the game with `setting_get` |
| `probe_plugin.c` | The plugin it loads: one of each hook, chained to the runtime's own handler and to the game's original. Also the smallest example of a plugin |
