"""What the issue bot does to a new issue (Moderator.on_issue in tools/github/moderate.py), against a stand-in for gh.
Why: the bot closes and labels real reports and had no test; only its reading (classify.py) was covered. Each case is
one decision a person would otherwise have to undo: who is left alone, what is closed, what is asked.

Usage: uv run pytest tests/github/test_moderator_issue.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/github"))

import moderate  # noqa: E402

TAGS = [f"v0.1.{i}" for i in range(14)]


class FakeGitHub(moderate.GitHub):
    """Records the gh commands the bot would run; the repo's releases and labels are canned."""

    def __init__(self) -> None:
        super().__init__("o/r", apply=False)
        self.calls: list[tuple[str, ...]] = []

    def get(self, path: str) -> object:
        if path.startswith("releases"):
            return [{"tag_name": t, "html_url": f"https://x/{t}", "draft": False} for t in TAGS]
        return [{"name": n} for n in ("duplicate", "needs-logs", "invalid", "performance", *TAGS, "gpu-nvidia")]

    def run(self, *args: str) -> None:
        self.calls.append(args)


def issue(title: str, body: str, *, assoc: str = "NONE", labels: tuple[str, ...] = ()) -> dict:
    return {
        "number": 900, "title": title, "body": body, "state": "open", "author_association": assoc,
        "labels": [{"name": n} for n in labels], "user": {"login": "player"},
    }  # fmt: skip


def run(iss: dict, comments: list[dict] | None = None) -> str:
    gh = FakeGitHub()
    moderate.Moderator(gh).on_issue(iss, comments or [])
    return " | ".join(" ".join(c) for c in gh.calls)


BUG = """### What happens

The game closes right after the start screen, it crashes every time I press play.

### Version

v0.1.13

### System

Windows 11 - NVIDIA GeForce RTX 3060
"""
DEVICE_FAULT = "\nvk: device fault: Instruction pointer invalid\n"  # signatures.toml, issue 22, fixed in v0.1.12


def test_a_maintainers_issue_is_left_alone() -> None:
    assert run(issue("Roadmap", BUG, assoc="OWNER")) == ""


def test_a_closed_issue_is_left_alone() -> None:
    assert run(issue("crash", BUG) | {"state": "closed"}) == ""


def test_an_empty_issue_is_closed_as_not_planned() -> None:
    out = run(issue("help", "### What happens\n\n_No response_\n"))
    assert "--add-label=invalid" in out
    assert "issue close 900 --reason=not planned" in out


def test_a_request_for_the_game_is_closed() -> None:
    out = run(issue("where to get the game", "Where can I download the rom or prod.keys for this?"))
    assert "--add-label=invalid" in out
    assert "issue close 900 --reason=not planned" in out


def test_a_bug_without_a_log_gets_its_labels_and_a_request_for_it() -> None:
    out = run(issue("Crash at start", BUG))
    assert "--add-label=v0.1.13" in out
    assert "--add-label=gpu-nvidia" in out
    assert "--add-label=needs-logs" in out
    assert "mkw-bot:logs" in out
    assert "issue close" not in out


def test_a_maintainer_reply_stops_the_bot_after_the_labels() -> None:
    """The bot never talks over a person: with a maintainer's reply it labels the release and nothing else, even for a
    known error."""
    reply = {"body": "Looking at it.", "author_association": "OWNER", "user": {"login": "maint"}}
    out = run(issue("Crash at start", BUG.replace("v0.1.13", "v0.1.7") + DEVICE_FAULT), [reply])
    assert "--add-label=v0.1.7" in out
    assert "issue close" not in out
    assert "mkw-bot" not in out


def test_a_known_error_closes_the_issue_as_a_duplicate_of_its_issue() -> None:
    out = run(issue("Crash at start", BUG.replace("v0.1.13", "v0.1.7") + DEVICE_FAULT))
    assert "--add-label=duplicate" in out
    assert "mkw-bot:dup" in out
    assert "issue close 900 --duplicate-of=22" in out


def test_a_fixed_error_on_the_fix_release_is_left_to_a_person() -> None:
    """On the release that fixed it or a later one, the same error may be a regression."""
    assert "issue close" not in run(issue("Crash at start", BUG + DEVICE_FAULT))


def test_an_edit_to_another_release_swaps_the_release_label() -> None:
    out = run(issue("Crash at start", BUG, labels=("v0.1.7",)))
    assert "--add-label=v0.1.13" in out
    assert "--remove-label=v0.1.7" in out
