# tests/widescreen

Window aspect, FOV and the UI fitting of ultrawide and tall windows.

| File | What it checks |
|---|---|
| `test_aspect.py` | Window aspect, horizontal FOV, UI fitting and unchanged 16:9/wide/version defaults |
| `test_vertplus.py` | The actual late race-FOV hook over repeated requests and tall render-target dimensions |
| `test_vertplus_gate.py` | Old game tables without the separate FOV set fail closed to 16:9 |
| `test_ui_capture.py` | Actual draw hook, full-canvas backdrops, character/kart masks, cup backdrop, ordinary controls and movies |
