"""The Discussions moderation's reading (tools/github/discuss.py, topics.toml) on the titles and texts players have
posted: a wrong topic or a wrong "bug report" closes a good question, so every discussion opened up to 17/11/2026 is a
case here, with what the bot should have done.

Usage: uv run pytest tests
"""

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/github"))

import discuss  # noqa: E402

TOPICS = discuss.load_topics()

# (number, title, body excerpt, topic id or "bug" or None)
CASES = [
    (3, "[Request] Support for version 1.8.0", "", "versions"),
    (8, "[Request] NUP format support", "", "versions"),
    (16, "multiplayer?", "Is there a way to play with two players?", "local-multiplayer"),
    (25, "Are we getting V1.8 Support?", "", "versions"),
    (42, "Add Brazilian Portuguese language option", "", None),
    (53, "Add .nup support on Setup", "", "versions"),
    (69, "Why not create a discord", "", "discord"),
    (82, "Avx2", "Meu processador não tem avx2", "avx2"),
    (90, "5.1 PCM surround sound", "", None),
    (113, "Native Nintendo Switch 2 Controller Support", "", None),
    (115, "not working when trying to play 2 players", "", "local-multiplayer"),
    (139, "me salio este error", "https://github.com/user-attachments/files/1/mkw.log", "bug"),
    (180, "Crash in 1st survival circuit ( V0.18 & RTX 5080)", "", "bug"),
    (182, "Solo puedo completar La copa Champiñón y la copa Hoja", "the game crashes on the third cup", "bug"),
    (193, "Add Thai language support for version 1.8.0", "", None),
    (194, "Please make an argument for direct play without having to use the launcher", "", "skip-launcher"),
    (202, "Dynamic FPS?", "", None),
    (228, "FEATURE WEBCAM", "", None),
    (233, "Skip Laucher", "", "skip-launcher"),
    (236, "Launcher Support", "Will it be in Quiver?", None),
    (246, "Is there any plan for online multiplayer?", "", "online"),
    (260, "Run game directly from command line", "", "skip-launcher"),
    (264, "Support for 48:9 aspect ratio (5760x1080 / 7680x1440)", "", None),
    (265, "slow motion", "3 and 4 players run in slow motion", "bug"),
    (285, "[Feature Request] 60fps on 4 player split screen", "", None),
    (288, "[Feature Request] Set Profile Name", "", None),
    (292, "Mirror Mode Unlock Option", "", None),
    (302, "Gyroscope of the second controller is not working", "", "bug"),
    (312, "How do I pre install shader cache?", "", "shader-cache"),
    (322, "Linux - Bypass the launcher and load the game directly.", "", "skip-launcher"),
    (330, "question about the camera?", "minimal stutters and slowdown !", None),
    (370, "Handheld V.S Docked performance on same internal resolution?", "", None),
    (399, "Compiling by command line on Linux?", "", None),
    (401, "Enable controller on the launcher menu", "", None),
    (407, "Updating usingvthe alredy compiled folder", "", "update"),
    (432, "Last version slowdowns, i only can play at 30fps now.", "", "bug"),
    (435, "[Feature Request] Add Cemuhook / DSU (UDP) Motion Client Support", "", None),
    (445, "Discord Rich Presence support?", "", None),
    (465, "THANK U FOR THIS PROJECT !!", "", None),
    (470, "Will online play ever get added?", "", "online"),
    (513, "Add an update feature to the recomp", "", "update"),
    (516, "[Feature Request] Controller Battery Percentage HUD / Status Support", "", None),
    (526, "Where is the save file located?", "", "save"),
    (531, "Adding More Performance Enhancing Mode To The Settings", "", None),
    (534, "Performance Issues and Visual Artifacts Since Version 1.12 (Worse in 1.13)", "artifacts appear", "bug"),
]


def reading(title: str, body: str) -> str | None:
    """What the bot answers, in its order: a FAQ topic first (its title is the question), then a bug report."""
    if t := discuss.topic(title, TOPICS):
        return t.id
    return "bug" if discuss.bug_report(title, body) else None


@pytest.mark.parametrize(("number", "title", "body", "want"), CASES, ids=[str(c[0]) for c in CASES])
def test_reading(number: int, title: str, body: str, want: str | None) -> None:
    assert reading(title, body) == want


def test_topics_are_complete() -> None:
    ids = [t.id for t in TOPICS]
    assert len(ids) == len(set(ids))
    for t in TOPICS:
        assert t.faq and t.reply and t.any, t.id


def test_forms_are_not_bug_reports() -> None:
    """The Q&A and Ideas forms' own words (.github/DISCUSSION_TEMPLATE) go into every body."""
    qa = "### Before asking\n\n- [X] I read the FAQ and searched the earlier discussions\n\n### Release\n\nv0.1.13"
    ideas = "### Before posting\n\n- [X] I searched the earlier ideas and the FAQ\n\n### Idea\n\nA ghost"
    for body in (qa, ideas):
        assert not discuss.bug_report("A question", body)
