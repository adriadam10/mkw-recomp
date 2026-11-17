# /// script
# requires-python = ">=3.12"
# ///
"""Moderates the GitHub Discussions: a bug report is sent to the issue forms (or to the issue of its known error) and a
question the FAQ answers gets its answer; both are closed, and their author's reply reopens them.

Why: of the first 59 discussions (17/11/2026), 13 were bug reports without their log and 20 repeated an answered
question; "open an issue with logs/mkw.log" was written by hand 11 times. It runs from .github/workflows/discussions.yml
on each new discussion and comment; by hand, it prints what it would do unless --apply.

Rules (never on maintainers' posts, nor where a maintainer or the bot already replied; only Q&A, General and Ideas):
- asks for the game or its keys: answered and closed
- a known error (signatures.toml) in its text or attached logs: "looks like #N", closed as a duplicate
- a question in topics.toml (by its title): the answer and its FAQ section, closed as a duplicate
- a failure or slowdown described: sent to the bug or performance form, closed
- its author replying after the bot closed it: reopened

Usage: uv run tools/github/moderate_discussions.py discussion N | comment N | backlog [--apply]
"""

import argparse
import json
import os
import subprocess

import classify
import discuss
from moderate import MAINTAINERS, MARK, attached_logs

FIELDS = """
  id number title body closed authorAssociation author { login } category { name }
  comments(first: 50) { nodes { body createdAt authorAssociation author { login }
    replies(first: 20) { nodes { body createdAt authorAssociation author { login } } } } }
"""
ONE = "query($o: String!, $r: String!, $n: Int!) { repository(owner: $o, name: $r) { discussion(number: $n) {%s} } }"
OPEN = (
    "query($o: String!, $r: String!) { repository(owner: $o, name: $r) "
    "{ discussions(first: 50, states: OPEN) { nodes {%s} } } }"  # 50 x 50 x 20 nodes: GraphQL's cap is 500,000
)
COMMENT = "mutation($id: ID!, $body: String!) { addDiscussionComment(input: {discussionId: $id, body: $body}) { clientMutationId } }"
CLOSE = "mutation($id: ID!, $why: DiscussionCloseReason!) { closeDiscussion(input: {discussionId: $id, reason: $why}) { clientMutationId } }"
REOPEN = "mutation($id: ID!) { reopenDiscussion(input: {discussionId: $id}) { clientMutationId } }"

TEXT = {
    "reopen": " If yours is a different question, reply here and it is reopened.",
    "topic": "{reply}\n\nIt is in the [FAQ]({repo_url}/wiki/FAQ#{anchor}){seen}. This discussion is closed as a "
    "duplicate.",
    "seen": ", and was asked first in #{seen}",
    "bug": "Thanks for the report. Bugs are followed in issues, where their log can be read: please open one with the "
    "[bug report form]({repo_url}/issues/new?template=bug_report.yml) (or the "
    "[performance form]({repo_url}/issues/new?template=performance.yml) for frame rate and slowdowns) and attach "
    "`logs/mkw.log` from the game folder ([where it is]({repo_url}/wiki/Installation#where-things-live)). Known "
    "problems and what to do about them are in [Troubleshooting]({repo_url}/wiki/Troubleshooting). Discussions are for "
    "questions and ideas, so this one is closed.",
    "dup": "Thanks for the report. This looks like #{issue}: {what}{fixed}. Please follow that issue, and add your "
    "`logs/mkw.log` there if it still happens on the [latest release]({repo_url}/releases/latest). This discussion "
    "is closed.",
    "fixed": ", fixed in {fixed}",
    "game": "Only Mario Kart World 1.0.0 or 1.8.0 dumped from your own console is supported; nothing of the game is "
    "shared here, and requests for it, its files or keys are closed.",
}


class GitHub:
    """GraphQL through gh; writes are printed instead of run unless apply."""

    def __init__(self, repo: str, apply: bool) -> None:
        self.owner, self.name = repo.split("/")
        self.repo_url = f"https://github.com/{repo}"
        self.apply = apply

    def query(self, q: str, **v: str | int) -> dict:
        args = ["gh", "api", "graphql", "-f", f"query={q}"]
        for k, val in v.items():
            args += ["-F" if isinstance(val, int) else "-f", f"{k}={val}"]
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"gh api graphql: {r.stderr.strip() or r.stdout.strip()}")
        return json.loads(r.stdout)["data"]

    def discussion(self, n: int) -> dict:
        return self.query(ONE % FIELDS, o=self.owner, r=self.name, n=n)["repository"]["discussion"]

    def open_discussions(self) -> list[dict]:
        return self.query(OPEN % FIELDS, o=self.owner, r=self.name)["repository"]["discussions"]["nodes"]

    def releases(self) -> set[str]:
        out = subprocess.run(
            [
                "gh",
                "api",
                f"repos/{self.owner}/{self.name}/releases?per_page=100",
                "-q",
                ".[] | select(.draft | not) | .tag_name",
            ],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        return set(out.split())

    def write(self, what: str, q: str, **v: str) -> None:
        print(f"  {what}")
        if self.apply:
            self.query(q, **v)

    def reply_and_close(self, d: dict, kind: str, text: str, reason: str) -> None:
        body = f"{text}{TEXT['reopen']}\n\n{MARK.format(kind)}"
        self.write(f"comment {kind}: {text[:90]}...", COMMENT, id=d["id"], body=body)
        self.write(f"close {reason}", CLOSE, id=d["id"], why=reason)


def login(node: dict) -> str:
    return (node.get("author") or {}).get("login", "ghost")


def answered(d: dict) -> bool:
    """A maintainer or the bot already replied: the bot never talks over a person, nor twice."""
    return any(c["authorAssociation"] in MAINTAINERS or "<!-- mkw-bot:" in c["body"] for c in posts(d))


def posts(d: dict) -> list[dict]:
    """Comments and their threaded replies, oldest first (#504: a maintainer's answer was only in a reply)."""
    out = [p for c in d["comments"]["nodes"] for p in (c, *c["replies"]["nodes"])]
    return sorted(out, key=lambda p: p["createdAt"])


def on_discussion(gh: GitHub, d: dict, releases: set[str], topics: list, sigs: list) -> None:
    n, title, body = d["number"], d["title"], d["body"] or ""
    if d["closed"] or d["category"]["name"] not in discuss.MODERATED or d["authorAssociation"] in MAINTAINERS:
        return
    if answered(d):
        return
    print(f"#{n} [{d['category']['name']}] {title[:70]}")
    url = gh.repo_url
    if classify.asks_for_game(title, body):
        gh.reply_and_close(d, "game", TEXT["game"], "RESOLVED")
        return
    logs = "\n".join(t for _, t in attached_logs(body))
    ver = classify.version(title, body, releases)
    if sig := classify.known_error(n, f"{title}\n{body}\n{logs}", ver, sigs):
        fixed = TEXT["fixed"].format(fixed=sig.fixed) if sig.fixed else ""
        gh.reply_and_close(
            d, "dup", TEXT["dup"].format(issue=sig.issue, what=sig.what, fixed=fixed, repo_url=url), "DUPLICATE"
        )
        return
    if t := discuss.topic(title, topics):
        seen = TEXT["seen"].format(seen=t.seen) if t.seen else ""
        reply = t.reply.format(repo_url=url)
        text = TEXT["topic"].format(reply=reply, anchor=t.faq, seen=seen, repo_url=url)
        gh.reply_and_close(d, f"topic-{t.id}", text, "DUPLICATE")
        return
    if discuss.bug_report(title, body):
        gh.reply_and_close(d, "bug", TEXT["bug"].format(repo_url=url), "RESOLVED")
        return
    print("  left to a person")


def on_comment(gh: GitHub, d: dict) -> None:
    """Its author's latest comment on a discussion the bot closed reopens it (the bot's reading was wrong, or there is
    more to it); a person reads it next."""
    comments = posts(d)
    if not d["closed"] or not comments or login(comments[-1]) != login(d):
        return
    if any(c["authorAssociation"] in MAINTAINERS for c in comments) or not any(
        "<!-- mkw-bot:" in c["body"] for c in comments
    ):
        return
    print(f"#{d['number']} {d['title'][:70]}: its author replied after the bot closed it")
    gh.write("reopen", REOPEN, id=d["id"])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("event", choices=["discussion", "comment", "backlog"])
    ap.add_argument("number", nargs="?", type=int)
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", "adriadam10/mkw-recomp"))
    ap.add_argument("--apply", action="store_true", help="act; without it, print what it would do")
    a = ap.parse_args()
    gh = GitHub(a.repo, a.apply)
    if a.event == "comment":
        on_comment(gh, gh.discussion(a.number))
        return
    releases, topics, sigs = gh.releases(), discuss.load_topics(), classify.load_signatures()
    todo = [gh.discussion(a.number)] if a.event == "discussion" else gh.open_discussions()
    for d in todo:
        on_discussion(gh, d, releases, topics, sigs)


if __name__ == "__main__":
    main()
