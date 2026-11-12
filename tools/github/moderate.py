# /// script
# requires-python = ">=3.12"
# ///
"""Moderates the GitHub issues: labels each with the release it reports and its GPU, closes the empty and the ones asking
for the game, asks for the log, answers known errors (signatures.toml) and closes what a maintainer's reply settles.

Why: by 11/11/2026, 85 issues in two days; replies saying "Fixed in v0.1.4" or "same as #36" left them open, the same
crashes came in several issues, and RecompLauncher's reports (no form) got no labels. It runs from
.github/workflows/issues.yml on each event and once a day; by hand, it prints what it would do unless --apply.

Rules (the bot never comments where a maintainer already did):
- empty (under 30 characters, no attachment) or asking for the game: closed as not planned
- a bug report without a log and no replies yet: `needs-logs`, closed as not planned after 7 days without a reply from its author
- a known error: `duplicate` and "looks like #N", closed as its duplicate after 3 days unless its author replies
- a maintainer's reply with "Fixed in vX": closed if vX is out, else `fixed-next-release`; with "same as #N": duplicate
- a closed issue is reopened when its author, or anyone attaching a log, says it still happens
- a published release closes the `fixed-next-release` issues with a link to it

Usage: uv run tools/github/moderate.py issue N | comment N COMMENT_ID | sweep | release TAG | backlog [--apply]
"""

import argparse
import json
import os
import re
import subprocess
import urllib.request
from datetime import UTC, datetime, timedelta

import classify

MAINTAINERS = {"OWNER", "MEMBER", "COLLABORATOR"}
MARK = "<!-- mkw-bot:{} -->"
DUP_DAYS, LOG_DAYS = 3, 7
NEW_LABELS = {
    "performance": ("fbca04", "Performance report: frame times on a player's machine"),
    "from-source": ("ededed", "Reported on a build from source"),
    "unknown-version": ("ededed", "The release could not be read from the report"),
}

TEXT = {
    "empty": "This issue has no details to act on, so it is closed. Please open a new one with the "
    "[bug report form]({repo_url}/issues/new?template=bug_report.yml) and attach `logs/mkw.log`.",
    "game": "Only Mario Kart World 1.0.0 or 1.8.0 dumped from your own console is supported; issues about obtaining the game, "
    "its files or keys are closed.",
    "logs": "Thanks for the report. To find the cause we need the log: please attach `logs/mkw.log` from the game "
    "folder (or `logs/setup.log` if the setup failed) by dragging it into a comment. Without it this issue is closed "
    f"in {LOG_DAYS} days.",
    "dup": "Thanks for the report. This looks like #{issue}: {what}. Please follow and add to that issue instead; this "
    f"one is closed as its duplicate in {DUP_DAYS} days unless you reply here.",
    "dup_old": "Thanks for the report. This looks like #{issue}: {what}, fixed in {fixed}. You are on {ver}: please "
    "update to the [latest release]({repo_url}/releases/latest). This one is closed as its duplicate in "
    f"{DUP_DAYS} days unless you reply here.",
    "logs_closed": f"Closed: no log in {LOG_DAYS} days. If it still happens on the latest release, open a new issue "
    "with `logs/mkw.log` attached.",
    "dup_closed": "Closed as a duplicate of #{issue}.",
    "modified": "Note: this log is from a build with local changes (`MKW Recomp {ver}`). Please check whether it "
    "happens with a [release package]({repo_url}/releases/latest) or a clean build of `main`: a change outside the "
    "repository can cause it, and we cannot see it.",
    "release": "Fixed in [{tag}]({url}). If it still happens there, open a new issue with `logs/mkw.log` attached.",
}


LOG_FILE = re.compile(r"https://github\.com/user-attachments/files/\d+/[^\s)\]\"']+\.(?:log|txt)")


def attached_logs(text: str) -> str:
    """The text of the .log/.txt files attached in text (the first 4, up to 4 MiB each). A known error is usually only
    in the log: #96 (11/11/2026) had "instruction pointer invalid" in mkw.1.log and nothing in its body."""
    out = []
    for url in list(dict.fromkeys(LOG_FILE.findall(text)))[:4]:
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                out.append(r.read(4 << 20).decode("utf-8", "replace"))
        except OSError as e:
            print(f"  {url}: {e}")
    return "\n".join(out)


class GitHub:
    """gh with GH_REPO set; writes are printed instead of run unless apply."""

    def __init__(self, repo: str, apply: bool) -> None:
        self.repo, self.apply = repo, apply
        self.env = {**os.environ, "GH_REPO": repo}

    def get(self, path: str) -> object:
        out = subprocess.run(
            ["gh", "api", "--paginate", "--slurp", f"repos/{self.repo}/{path}"],
            capture_output=True,
            text=True,
            check=True,
            env=self.env,
        ).stdout
        pages = json.loads(out)
        return [x for p in pages for x in p] if pages and isinstance(pages[0], list) else pages[0]

    def run(self, *args: str) -> None:
        print("  " + " ".join(a if len(a) < 60 else a[:57] + "..." for a in args))
        if self.apply:
            subprocess.run(["gh", *args], check=True, env=self.env, stdout=subprocess.DEVNULL)

    def label(self, n: int, *labels: str) -> None:
        self.run("issue", "edit", str(n), *(f"--add-label={lb}" for lb in labels))

    def unlabel(self, n: int, *labels: str) -> None:
        self.run("issue", "edit", str(n), *(f"--remove-label={lb}" for lb in labels))

    def comment(self, n: int, kind: str, text: str) -> None:
        self.run("issue", "comment", str(n), "--body", f"{text}\n\n{MARK.format(kind)}")

    def close(self, n: int, reason: str, text: str | None = None, dup_of: int | None = None) -> None:
        args = ["issue", "close", str(n)]
        args += [f"--duplicate-of={dup_of}"] if dup_of else [f"--reason={reason}"]
        self.run(*args, *(["--comment", text] if text else []))


class Moderator:
    def __init__(self, gh: GitHub) -> None:
        self.gh = gh
        self.repo_url = f"https://github.com/{gh.repo}"
        rel = gh.get("releases?per_page=100")
        self.releases = {r["tag_name"]: r["html_url"] for r in rel if not r["draft"]}
        self.sigs = classify.load_signatures()
        self.labels = {lb["name"] for lb in gh.get("labels?per_page=100")}

    def ensure_label(self, name: str) -> None:
        if name in self.labels:
            return
        color, desc = NEW_LABELS.get(name, ("c2e0c6", f"Reported on {name}"))
        self.gh.run("label", "create", name, "--color", color, "--description", desc)
        self.labels.add(name)

    def comments(self, n: int) -> list[dict]:
        return self.gh.get(f"issues/{n}/comments?per_page=100")

    def on_issue(self, issue: dict, comments: list[dict]) -> None:
        """Labels and first answer of a new or edited issue."""
        n, title, body = issue["number"], issue["title"], issue["body"] or ""
        if issue["state"] != "open" or issue["author_association"] in MAINTAINERS:
            return
        have = {lb["name"] for lb in issue["labels"]}
        print(f"#{n} {title[:70]}")
        human = any(c["author_association"] in MAINTAINERS for c in comments)
        said = "\n".join(c["body"] for c in comments)
        # The log often comes in a later comment of its author
        reported = "\n".join([body, *(c["body"] for c in comments if c["user"]["login"] == issue["user"]["login"])])
        if classify.is_empty(title, body) and not human:
            self.gh.label(n, "invalid")
            self.gh.close(n, "not planned", TEXT["empty"].format(repo_url=self.repo_url))
            return
        if classify.asks_for_game(title, body):
            self.gh.label(n, "invalid")
            self.gh.close(n, "not planned", TEXT["game"])
            return
        ver = classify.version(title, body, set(self.releases)) or "unknown-version"
        add = {ver} - have
        stale = {lb for lb in have if (lb[:1] == "v" and lb[1:2].isdigit()) or lb in NEW_LABELS} - {ver, "performance"}
        if not have & set(classify.GPU_PATTERNS) and (g := classify.gpu(title, body)):
            add.add(g)
        if title.lower().startswith("perf:"):
            add.add("performance")
        for lb in add:
            self.ensure_label(lb)
        if add:
            self.gh.label(n, *sorted(add))
        if stale:
            self.gh.unlabel(n, *sorted(stale))
        if human:
            return
        logs = attached_logs(reported)
        if (mod := classify.modified_build(logs)) and MARK.format("modified") not in said:
            self.gh.comment(n, "modified", TEXT["modified"].format(ver=mod, repo_url=self.repo_url))
        sig = classify.known_error(n, f"{title}\n{body}\n{logs}", ver, self.sigs)
        if sig and MARK.format("dup") not in said:
            key = "dup_old" if sig.fixed else "dup"
            text = TEXT[key].format(issue=sig.issue, what=sig.what, fixed=sig.fixed, ver=ver, repo_url=self.repo_url)
            self.gh.label(n, "duplicate")
            self.gh.comment(n, "dup", text)
        elif "needs-logs" in have and classify.has_log(reported):
            self.gh.unlabel(n, "needs-logs")
        elif not sig and not comments and classify.wants_log(title, reported, have | add):
            self.gh.label(n, "needs-logs")
            self.gh.comment(n, "logs", TEXT["logs"])

    def on_comment(self, issue: dict, c: dict) -> bool:
        """A maintainer's reply that settles the issue, or its author answering the bot. True if it closed it."""
        n = issue["number"]
        have = {lb["name"] for lb in issue["labels"]}
        if issue["state"] != "open":
            # #38 (11/11/2026): closed by a "fixed in v0.1.7" reply, then "same error in v0.1.7" with a new log
            mine = c["user"]["login"] == issue["user"]["login"]
            if c["author_association"] not in MAINTAINERS and classify.still_happens(c["body"], mine):
                print(f"#{n} {issue['title'][:70]}: still happens, reopened")
                self.gh.run("issue", "reopen", str(n))
                if "fixed-next-release" in have:
                    self.gh.unlabel(n, "fixed-next-release")
            return False
        if c["author_association"] in MAINTAINERS:
            decision = classify.reply_decision(c["body"])
            if not decision:
                return False
            print(f"#{n} {issue['title'][:70]}: {decision}")
            kind, arg = decision
            if kind == "duplicate":
                self.gh.close(n, "duplicate", dup_of=int(arg))
                return True
            if arg in self.releases:
                self.gh.close(n, "completed")
                return True
            if "fixed-next-release" not in have:
                self.gh.label(n, "fixed-next-release")
            return False
        if c["user"]["login"] == issue["user"]["login"]:
            drop = {"duplicate"} & have
            if "needs-logs" in have and classify.has_log(c["body"]):
                drop.add("needs-logs")
            if drop:
                print(f"#{n}: its author replied")
                self.gh.unlabel(n, *sorted(drop))
        return False

    def labeled_at(self, n: int, label: str) -> datetime | None:
        events = [e for e in self.gh.get(f"issues/{n}/events?per_page=100") if e["event"] == "labeled"]
        times = [e["created_at"] for e in events if e["label"]["name"] == label]
        return datetime.fromisoformat(times[-1]) if times else None

    def sweep(self, issue: dict, comments: list[dict]) -> None:
        """Closes what waited long enough for its author: the log, or an answer to "looks like #N"."""
        n = issue["number"]
        have = {lb["name"] for lb in issue["labels"]}
        now = datetime.now(UTC)
        for label, days in (("duplicate", DUP_DAYS), ("needs-logs", LOG_DAYS)):
            if label not in have or not (since := self.labeled_at(n, label)) or now - since < timedelta(days=days):
                continue
            author = issue["user"]["login"]
            if any(c["user"]["login"] == author and datetime.fromisoformat(c["created_at"]) > since for c in comments):
                continue
            print(f"#{n} {issue['title'][:70]}: {label} since {since:%d/%m %H:%M}")
            if label == "duplicate":
                bot = "\n".join(c["body"] for c in comments if MARK.format("dup") in c["body"])
                if m := re.search(r"looks like #(\d+)", bot):
                    dup = int(m.group(1))
                    self.gh.close(n, "duplicate", TEXT["dup_closed"].format(issue=dup), dup_of=dup)
                    return
            else:
                self.gh.close(n, "not planned", TEXT["logs_closed"])
                return

    def release(self, tag: str) -> None:
        url = self.releases.get(tag) or f"{self.repo_url}/releases/tag/{tag}"
        for issue in self.gh.get("issues?state=open&labels=fixed-next-release&per_page=100"):
            print(f"#{issue['number']} {issue['title'][:70]}: {tag}")
            self.gh.unlabel(issue["number"], "fixed-next-release")
            self.gh.close(issue["number"], "completed", TEXT["release"].format(tag=tag, url=url))

    def open_issues(self) -> list[dict]:
        return [i for i in self.gh.get("issues?state=open&per_page=100") if "pull_request" not in i]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("event", choices=["issue", "comment", "sweep", "release", "backlog"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", "adriadam10/mkw-recomp"))
    ap.add_argument("--apply", action="store_true", help="act; without it, print what it would do")
    a = ap.parse_args()
    m = Moderator(GitHub(a.repo, a.apply))
    if a.event == "issue":
        n = int(a.args[0])
        m.on_issue(m.gh.get(f"issues/{n}"), m.comments(n))
    elif a.event == "comment":
        n, cid = int(a.args[0]), int(a.args[1])
        m.on_comment(m.gh.get(f"issues/{n}"), m.gh.get(f"issues/comments/{cid}"))
    elif a.event == "release":
        m.release(a.args[0])
    else:
        for issue in m.open_issues():
            comments = m.comments(issue["number"])
            if a.event == "backlog":
                # The last reply of a maintainer that settles it, unless a player wrote after it (#38: "same error in
                # v0.1.7" under "fixed in v0.1.7"); then labels and first answer like a new issue
                settled = [
                    i
                    for i, c in enumerate(comments)
                    if c["author_association"] in MAINTAINERS and classify.reply_decision(c["body"])
                ]
                after = settled and any(c["author_association"] not in MAINTAINERS for c in comments[settled[-1] + 1 :])
                if settled and not after and m.on_comment(issue, comments[settled[-1]]):
                    continue
                m.on_issue(issue, comments)
            m.sweep(issue, comments)


if __name__ == "__main__":
    main()
