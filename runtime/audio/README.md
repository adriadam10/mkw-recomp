# runtime/audio

| File | What it is |
|---|---|
| `audren.c`, `audren.h` | Audio renderer behind the SDK's `nn::audio` client, with Switch 2's `SetVoiceDropParameter`; RequestUpdate keeps voice state/status by input slot, so unused zeroed records do not reset voice 0 (`tests/test_audio_update.py`) |
| `audren_sf.c` | The client's service objects (`IAudioRendererManager`, `IAudioRenderer`, `IAudioDevice`) made by the host: `nn::audio::CreateAudioRendererManager(bool)`, replaced by name, returns an object whose virtual table is host functions (`HOST_FUNCTION`); no IPC. The ABI of `nn::sf` objects is in its header |
| `audio_mixer.c`, `audio_effects.*`, `audio_reverb.*` | Mixing and effects; output through SDL3; `IAudioDevice` with Switch 2's commands 19/20. Diagnostics with `MKW_TRACE=audio` (level, underruns, voices run dry, pauses of voices that were playing) and `MKW_AUDIO_RAW=<path>` (output as stereo float32) |
