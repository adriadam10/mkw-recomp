"""Reads a new discussion without GitHub: a bug report that belongs in an issue (a known error from signatures.toml, or a
failure described), or a question the FAQ already answers (topics.toml). moderate_discussions.py acts on it;
tests/github/test_discussions.py checks it on every discussion opened so far.

Why: of the first 59 discussions (17/11/2026), 13 were bug reports without their log and 20 repeated a question already
answered (skipping the start screen six times, 1.8.0 four, online play three), each answered by hand.
"""

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path

import classify

TOPICS = Path(__file__).with_name("topics.toml")

# Categories the bot reads: Announcements, Polls and Show and tell are ours or contributors'
MODERATED = {"Q&A", "General", "Ideas"}
# A request names the failure it wants gone ("make it not crash"): only its title decides it is not a bug report
REQUEST = re.compile(r"^\s*\[?(feature|request|idea|suggestion)|^\s*(add|support|please add|allow|option)\b", re.I)
# Read only in the title: slowdowns, reports too for the performance form (#432 "Last version slowdowns", #265 "slow
# motion"; praise says "minimal stutters and slowdown", #330), and "not working" (#302), which classify.FAILURE misses
TITLE_FAILURE = re.compile(
    r"\b(slow\w*|stutter\w*|lag(gy|s|ging)?|fps drops?|drops? to \d+|only \d+ ?fps|not working)\b", re.I
)


@dataclass(frozen=True)
class Topic:
    id: str
    seen: int
    faq: str
    reply: str
    any: tuple[re.Pattern, ...]
    not_: tuple[re.Pattern, ...]

    def matches(self, title: str) -> bool:
        return any(p.search(title) for p in self.any) and not any(p.search(title) for p in self.not_)


def load_topics(path: Path = TOPICS) -> list[Topic]:
    rows = tomllib.loads(path.read_text())["topic"]
    return [
        Topic(
            r["id"],
            r["seen"],
            r["faq"],
            r["reply"],
            tuple(re.compile(p, re.I) for p in r["any"]),
            tuple(re.compile(p, re.I) for p in r.get("not", [])),
        )
        for r in rows
    ]


def topic(title: str, topics: list[Topic]) -> Topic | None:
    """The first FAQ topic the title asks about. Only the title: a body names the launcher, versions or online play in
    passing (#148, LAN play, explains why it is not online)."""
    return next((t for t in topics if t.matches(title)), None)


def bug_report(title: str, body: str) -> bool:
    """A failure on the player's machine: described in the title, or in the text of a post that is not a request.
    A log attached counts as a report on its own (#139: only a log link and "me salio este error")."""
    if REQUEST.search(title):
        return False
    text = f"{title}\n{classify.FENCE.sub('', body or '')}"
    return bool(
        classify.FAILURE.search(text) or TITLE_FAILURE.search(title) or classify.FILE_ATTACHMENT.search(body or "")
    )
