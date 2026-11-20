# tools/ffx/provider

Tracked support for the optional diagnostic provider copied into the generated
`build/ffx` work tree by `builder/build.py --provider`. The default production build
compiles one game-field shader directly from the pinned SDK source.

| File | Purpose |
|---|---|
| `CMakeLists.txt` | Defines the MSVC shared-library target and its pinned source inputs |
| `provider_glue.cpp` | Registers the FSR 3.1.6 frame-generation provider |
| `swapchain_stub.cpp` | Fails the unused swapchain entry point cleanly |
