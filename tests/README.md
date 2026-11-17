# tests

Tests that need neither the game nor a GPU, run by the CI on Linux and Windows (`.github/workflows/ci.yml`). The back
end's regression test runs over captures instead (`tools/test/`).

Each folder holds the tests of one part of the repo, with the C sources they build next to them:

| Folder | What it checks |
|---|---|
| [translator/](translator/README.md) | The shader translator, the SPIR-V patcher and the recompiler's code patches |
| [launcher/](launcher/README.md) | The setup, the installation selector and Add to Steam |
| [runtime/](runtime/README.md) | Pieces of `runtime/` built alone: audio, controllers, memory caps, settings, menu texts |
| [widescreen/](widescreen/README.md) | Window aspect, FOV and UI fitting |
| [github/](github/README.md) | The issue moderation bot |

Usage: `uv run pytest tests` (one worker per core through pytest-xdist; `-n 0` runs them in series)
