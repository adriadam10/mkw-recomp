# github

Issue moderation (`moderate.py`, `classify.py`), run by `.github/workflows/issues.yml` on each issue event, once a day and on each release. Discussion moderation (`moderate_discussions.py`, `discuss.py`), run by `.github/workflows/discussions.yml` on each new discussion and comment. The copy to the Internet Archive (`archive.py`) runs from `.github/workflows/archive.yml` once a day.

| File | Contents |
|---|---|
| `moderate.py` | Acts on an issue, a comment, a release or the daily sweep: release and GPU labels, empty reports closed, logs asked for, known errors answered, a log from a build with local changes (`-dirty`) noted, a known error only in an older log (`mkw.N.log`) or only the setup's warm-up log attached noted without closing, maintainers' replies closed. Prints what it would do unless `--apply` |
| `classify.py` | Reads a report without GitHub: release, GPU, empty, log, known error, and what a maintainer's reply decides (tested in `tests/github/test_issues.py`) |
| `archive.py` | The daily copy to the Internet Archive (`.github/workflows/archive.yml`): git bundle and every issue, pull request, discussion and release as JSON in a dated item, each release's files once, and the pages changed in the last three days to the Wayback Machine (what archive.org refuses one day is retried on the next two). Prints what it would do unless `--apply` |
| `moderate_discussions.py` | Acts on a new discussion in Q&A, General or Ideas, unless a maintainer posted it or already replied: a known error (`signatures.toml`) or a described failure or slowdown is sent to the issue forms, a question in `topics.toml` gets its answer and FAQ section; both are closed, and their author's reply reopens them. Prints what it would do unless `--apply` |
| `discuss.py` | Reads a discussion without GitHub: its FAQ topic by the title, whether it is a bug report (tested in `tests/github/test_discussions.py`) |
| `topics.toml` | The questions the Discussions keep getting: title patterns, the answer, its section in the [wiki's FAQ](https://github.com/adriadam10/mkw-recomp/wiki/FAQ) and the discussion that asked it first |
| `signatures.toml` | Known error strings and the issue that tracks each; `fixed` keeps reports on that release or later open as possible regressions; `wiki` is its section in the [wiki's Troubleshooting](https://github.com/adriadam10/mkw-recomp/wiki/Troubleshooting) page, which the bot's reply links |

A known error closes the new issue at once as its duplicate (its author's reply reopens it). A report on a release
older than the newest two, quiet for 3 days, is asked to try the latest (`needs-retry`) and closed after 7 days without
its author's reply. The sweep runs every 6 hours.

A maintainer's reply closes its issue when it says "Fixed in vX" opening a sentence (or "is the same as #N, fixed in
vX") with vX published, or "is the same as #N" (closed as its duplicate). "Fixed in" a release not yet out labels it
`fixed-next-release`, closed when that release is published. A reply only settles an issue if no player wrote after
it, and a closed issue is reopened when its author, or anyone attaching a log, says it still happens.

Discussions: `uv run tools/github/moderate_discussions.py backlog` (all open ones, dry run), `... discussion N --apply`;
from GitHub, `gh workflow run discussions.yml` (dry run by default; `-f dry_run=false` acts). The Q&A, Ideas and General
forms are in `.github/DISCUSSION_TEMPLATE/`.

Usage: `uv run tools/github/moderate.py backlog` (all open issues, dry run), `... issue N --apply`; from GitHub,
`gh workflow run issues.yml -f task=backlog` (or `sweep`, `issue -f issue=N`; `-f dry_run=true` only prints).
