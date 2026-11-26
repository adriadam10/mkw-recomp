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
| [build/](build/README.md) | The Windows dependency setup (`tools/build/windeps.py`): a package member never writes outside its deps folder |
| [github/](github/README.md) | The issue moderation bot |
| [run/](run/README.md) | The game-run tools (`tools/run`): `run.py` against a fake `mkw`, and the A/B verdict |
| [docs/](docs/README.md) | The pre-commit hook's own checks reading UTF-8 on a legacy code page |

Usage: `uv run pytest tests` (one worker per core through pytest-xdist; `-n 0` runs them in series)
