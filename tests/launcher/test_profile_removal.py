"""Profile removal changes the selected player only if the folder move succeeds."""

import shutil
import subprocess
from pathlib import Path

import probe_build
import pytest

ROOT = Path(__file__).resolve().parents[2]

STUBS = r"""
#include <cstdio>
#include <cstdlib>
#include <cstring>
enum { UI_PLAYER_CONFIRM, UI_PLAYER_REMOVE, UI_LAUNCH_BACK, ImGuiWindowFlags_AlwaysAutoResize };
namespace ImGui {
bool BeginPopupModal(const char *, void *, int) { return true; }
float GetFontSize() { return 15; }
void PushTextWrapPos(float) {}
void TextUnformatted(const char *) {}
void PopTextWrapPos() {}
bool Button(const char *label) { return !strcmp(label, "Remove"); }
void CloseCurrentPopup() {}
void SameLine() {}
void EndPopup() {}
}
static struct {
    int n, failed;
    const char *base, *removing;
    struct { const char *id, *name; } list[1];
} P;
static const char *active;
static bool can_remove;
const char *active_id() { return active; }
void pick(const char *id) { active = id; }
int player_remove(const char *, const char *) { return can_remove; }
void refresh() {}
const char *menu_text(int id) {
    if (id == UI_PLAYER_CONFIRM) return "Remove %s?";
    return id == UI_PLAYER_REMOVE ? "Remove" : "Back";
}
"""

DRIVER = r"""
int main(int argc, char **argv) {
    P.n = 1; P.base = "profiles"; P.removing = "2"; P.list[0] = {"2", "Second"};
    active = argv[1];
    can_remove = atoi(argv[2]) != 0;
    remove_modal();
    printf("%d:%s\n", P.failed, active);
}
"""


@pytest.fixture(scope="module")
def removal_probe(tmp_path_factory):
    compiler = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if not compiler:
        pytest.skip("C++ compiler needed")
    folder = tmp_path_factory.mktemp("player-removal")
    text = (ROOT / "runtime/ui/launcher/launcher_players.cpp").read_text(encoding="utf-8")
    start = text.index("static void remove_modal(")
    end = text.index("static void player_row(", start)
    source, exe = folder / "removal.cpp", folder / "removal.exe"
    source.write_text(STUBS + text[start:end] + DRIVER, encoding="utf-8")
    probe_build.build([compiler, str(source), "-o", str(exe)])
    return exe


@pytest.mark.parametrize(("active", "result", "expected"), [
    ("2", "0", "1:2"), ("2", "1", "0:"), ("3", "0", "1:3"), ("3", "1", "0:3"),
])
def test_select_default_only_after_removing_active_profile(removal_probe, active, result, expected):
    p = subprocess.run([str(removal_probe), active, result], check=True, capture_output=True, text=True, timeout=30)
    assert p.stdout.strip() == expected
