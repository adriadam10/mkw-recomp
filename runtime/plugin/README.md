# runtime/plugin

Native plugins: shared libraries a mod ships in its `plugin/` folder (`mods/<mod>/plugin/*.so` on Linux, `*.dll` on
Windows), loaded at start when the player has the mod enabled on the Mods page (or with `--mod <name>`), before the
game code loads. The API is generic: overrides by symbol and by guest address, a callback once the game is loaded,
calls into guest code, notices and a drawing of its own over the picture; what a plugin does with them is its own
business, and nothing of it lives in this repo. The software keyboard's number pad is the runtime's
(`ui/menu/text_entry.h`), so a plugin needs no text entry of its own.

| File | What it is |
|---|---|
| `mkw_plugin.h` | The API, version 4 (version 1, plus the language, the player, the mod's settings and its options page `MkwPage` in 2, images the plugin fills for the game's native image handles in 3, the pad filter, `menu_open`, the shown picture, and `focus_set` / `quit` for a plugin's overlay over the game in 4): the one header a plugin includes (it links against nothing of mkw-recomp). Rules for threads, guest memory and what each hook may do are in its comments |
| `plugin_overlay.cpp` | The plugins' drawings over the picture (`overlay_set`, `overlay_show`): rectangles and text on ImGui's background list in the menu overlay's frame, under the menu, the number pad and the notices |
| `mod_page_ui.cpp`, `mod_page_ui.h` | A mod's page (`MkwPage`) drawn with ImGui: headings, text, notes, checkboxes, text fields, choices, sliders, buttons, lists with buttons per row, columns, greyed-out sections; bound to the mod's settings. Used by the start screen's options page (`ui/launcher/mod_page.cpp`) and the mods' tabs of the in-game menu's settings (`settings_tab`, `ui/menu/settings_tabs.cpp`) |
| `mod_settings.c`, `mod_settings.h` | A mod's settings per player: `<player folder>/mod-settings/<mod>.ini`, one `key=value` per line, written atomically. The store behind the API's `setting_get` / `setting_set` (version 2) and the options page's widgets (`ui/launcher/mod_page.cpp`); outside the mod's folder, so updating or importing the mod again keeps them. Test: `tests/runtime/test_mod_settings.py` |
| `plugin.c`, `plugin.h` | The loader: each enabled mod's `plugin/` libraries, their `mkw_plugin_init`, the API table, the game-loaded callbacks. A plugin that declines or fails to load is logged and shown to the player in a box |

How the hooks reach the game: an override by symbol goes in as the loader binds the modules (`core/boot/loader.c`,
the game's own definition kept as its original); an override by address empties the function's slot of the dispatch
table (`cpu/dispatch.c` `rt_dispatch_override`), so every indirect call to it lands in the plugin, while direct calls
inside translated code stay direct. The test is `tools/plugin/probe.sh` (`build/mkw --probe-plugin`).
