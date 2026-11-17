# docs/images

Images for the main README: the logo, and screenshots taken from the start screen itself with its test hooks (`runtime/ui/launcher`):
`MKW_LAUNCHER_SHOT=<frame>:<page>` saves the window at that frame to `MKW_LAUNCHER_SHOT_OUT`, and `MKW_MENU_LANG=en`
sets the language. At 1280 x 720, compressed with `pngquant` and `oxipng`; the game folder shown is a neutral path
(`/mnt/MarioKartWorld`, a bind mount), not a personal one.

| File | What it shows |
|---|---|
| `launcher-main.png` | The main page once the game is set up, with the link to the project's GitHub page at the lower right (first by @ViniciusStanula, from #70) |
| `launcher-files.png` | The Game files page before the setup, cropped to its content |
| `logo.svg` | The repo's logo, the same chequered flag as `mkw.exe`'s icon, drawn by `tools/build/icon.py docs/images/logo.svg` |
