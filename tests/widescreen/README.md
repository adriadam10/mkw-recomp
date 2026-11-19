# tests/widescreen

Window aspect, FOV and the UI fitting of ultrawide and tall windows.

| File | What it checks |
|---|---|
| `test_aspect.py` | Window aspect, horizontal FOV, UI fitting and unchanged 16:9/wide/version defaults |
| `test_vertplus.py` | The actual late race-FOV hook over repeated requests and tall render-target dimensions |
| `test_display.py` | 1.8.0's display size (set display): its table sites and the real hooks, rows on a taller output, columns on a wider one, shrunk to fit the output |
| `test_vertplus_gate.py` | Old game tables without the separate FOV set fail closed to 16:9 |
| `test_list_masks.py` | The pane-matrix hook (1.8.0): its sites; on a wider output the character and kart lists' capture masks (the outer one to the screen edge, every one drawn wide where it was); the race and pause prompts' corner and 16:9 shape on wider and taller outputs; other panes and split screens left alone |
| `test_hud.py` | The HUD and the menus' chrome at the screen edges (`MKW_HUD_POSITION=edges`): the run-time layout signature against `recomp/hud_layouts.py`, anchors scoped by layout and depth, rest zones, bands and their children, the cup select's band as one group, the minimaps and their icons, split screens, unknown layouts and other classes left alone, the default 16:9 |
| `test_hud_layouts.py` | The reviewed layout classes (`recomp/hud-classes-180.tsv`): every 1.8.0 layout classified once, split roots in `split`, anchors only in `hud` and `menu` layouts, code-read panes and capture masks never anchored, colliding signatures share a class, the generated header lists every layout once |
| `test_ui_capture.py` | Actual draw hook, full-canvas backdrops, character/kart masks, cup backdrop, ordinary controls and movies |
