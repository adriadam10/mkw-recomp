# tests/github

The issue and discussion moderation bots (`tools/github/`).

| File | What it checks |
|---|---|
| `test_issues.py` | The issue moderation's reading of a report (`tools/github/classify.py`): the release from the form, RecompLauncher's report or the prose, the GPU without the CPU's vendor, empty reports, logs, known errors and their regressions, which maintainer replies close an issue and which player replies reopen it |
| `test_discussions.py` | The discussion moderation's reading (`tools/github/discuss.py`, `topics.toml`) on every discussion posted up to 17/11/2026: which FAQ topic a title asks, which posts are bug reports, and that the forms' own words are not |
