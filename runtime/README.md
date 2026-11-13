# runtime

What plays the console for the generated code. It started as the runtime of totk-pc-recomp on its Linux port branch
([PR #60](https://github.com/valentderah/totk-pc-recomp/pull/60), `wdiasjunior/linux-port` @ eccf4dd, MIT, license in
`LICENSE.totk-pc-recomp`; they adapted it from LAS Recomp, MacNativeGames and d2r-mobile, details in their
`THIRD_PARTY_NOTICES.md`), split by responsibility. It came from totk-pc-recomp (Switch 1, NVN1) and has been adapted
to MKW (Switch 2, NVN2). Left out of the copy: the Windows icons and totk-pc-recomp's `resolution.c` and
`frame_rate.c` patches. "TotK" in a comment means Tears of the Kingdom in totk-pc-recomp: the measurement or behaviour
behind that code was seen there, not in MKW.

| Folder | Contents |
|---|---|
| [cpu/](cpu/README.md) | Guest CPU state, headers of the generated C, indirect call dispatch and host functions |
| [core/](core/README.md) | Boot, module loader, memory, virtual cores, SVCs, settings, profiling |
| [sdk/](sdk/README.md) | `nn::*` functions replaced by name: system, files and RomFS, video, software keyboard |
| [audio/](audio/README.md) | The SDK's audio renderer and its effects |
| [input/](input/README.md) | Controllers, keyboard, mouse, gyro |
| [steam/](steam/README.md) | Steam Remote Play Together (off unless `steam_appid` is set): guests' controllers and keyboards as players |
| [gfx/](gfx/README.md) | NVN over Vulkan: API front end (`nvn/`) and back end (`vk/`) |
| [platform/](platform/README.md) | Operating system layer: `linux.c`, `win32.c` |
| [ui/](ui/README.md) | Menu and overlay (Dear ImGui) |
| [shaders/](shaders/README.md) | The runtime's own shaders (ASTC, FSR 1) |
| [probes/](probes/README.md) | Self-tests per subsystem |
| [third_party/](third_party/README.md) | Dear ImGui, bc7enc |
