# runtime/sdk/ov

Replacements of `nn::*` functions by name: system, input, audio, video, graphics, network, time and touch.

| File | What it is |
|---|---|
| `ov_system.c` | Accounts, applets, time, system fonts, etc. |
| `ov_input.c`, `ov_media.c`, `ov_gfx.c`, `ov_zstd.c` | Input, Opus, the SDK's graphics (`nv::InitializeGraphics(0, 0)` is valid on Switch 2; `nv::InitCheck`), native zstd |
| `ov_codec.c` | MKW's video: the `nn::codec` video API (VP9 with libvpx, decoded in `Submit`) and the `nn::syncpt` events as the game's `SystemEvent` |
| `ov.h` | Header shared by the replacements and their test (`ov_probe.c`) |
| `ov_touch.c` | MKW's own: touch screen off (optional), reads without touches |
| `ov_oe2.c` | MKW's own: the `nn::oe` that totk-pc-recomp does not replace (version 1.0.0, graphics scale, notifications) |
| `ov_chat.c` | MKW 1.8.0's `nn::chat` (GameChat): a console where no chat is started; the room-info event is real and never signalled |
| `ov_net.c` | MKW's own: offline network (`nifm`, `friends`, `ldn`, `nsd`, `prepo`, `socket`) |
| `ov_time2.c` | MKW's own: the `nn::time` that totk-pc-recomp does not replace (exact network clock, host time zone, instant clock correction) |
