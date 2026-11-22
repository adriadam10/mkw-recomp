# tests/github

The issue and discussion moderation bots (`tools/github/`).

| File | What it checks |
|---|---|
| `test_toml_utf8.py` | The bot's signatures.toml and topics.toml read as UTF-8 under a legacy code page: no mojibake in the replies |
| `test_issues.py` | The issue moderation's reading of a report (`tools/github/classify.py`): the release from the form, RecompLauncher's report or the prose, the GPU without the CPU's vendor, empty reports, logs, known errors and their regressions, which maintainer replies close an issue and which player replies reopen it |
| `test_moderator_issue.py` | What the issue bot does to a new issue (`moderate.py`, against a stand-in for gh): who is left alone, what is closed as empty, for the game or as a duplicate, what is asked for |
| `test_discussions.py` | The discussion moderation's reading (`tools/github/discuss.py`, `topics.toml`) on every discussion posted up to 17/11/2026: which FAQ topic a title asks, which posts are bug reports, and that the forms' own words are not |
| `test_gh_encoding.py` | The bots read `gh`'s JSON as UTF-8 on a legacy code page (Windows 1252, the C locale), with a stand-in for `subprocess.run` that decodes the way the call asks |
