"""Check the launcher updater's handling of manual and automatic release checks without a live server."""

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
#include "launcher_update.h"
#define SDLCALL
using SDL_Mutex = int;
using SDL_Process = int;
using SDL_IOStream = int;
static SDL_Mutex mutex;
static SDL_Process process;
static SDL_IOStream output;
static const char *lines;
static int exit_code, launches;
static bool auto_check, packaged = true, spawn_ok = true;
static const char *rt_port_version = "0.1.15";
SDL_Mutex *SDL_CreateMutex() { return &mutex; }
void SDL_LockMutex(SDL_Mutex *) {}
void SDL_UnlockMutex(SDL_Mutex *) {}
const char *SDL_GetError() { return "could not spawn"; }
SDL_IOStream *SDL_GetProcessOutput(SDL_Process *) { return &output; }
size_t SDL_ReadIO(SDL_IOStream *, char *out, size_t) {
    if (!*lines) return 0;
    *out = *lines++;
    return 1;
}
enum { SDL_IO_STATUS_NOT_READY = 1 };
int SDL_GetIOStatus(SDL_IOStream *) { return 0; }
void SDL_Delay(int) {}
void SDL_WaitProcess(SDL_Process *, bool, int *code) { *code = exit_code; }
void SDL_DestroyProcess(SDL_Process *) {}
bool SDL_GetPathInfo(const char *, void *) { return false; }
void rt_log(const char *, ...) {}
bool cfg_bool(const char *) { return auto_check; }
const char *cfg_game_dir() { return "game"; }
bool installable(const char *, char *, size_t) { return packaged; }
bool find_tools(const char *, char *python, char *script, size_t) {
    strcpy(python, "python");
    strcpy(script, "update.py");
    return packaged;
}
int setup_step_index(const char *) { return -1; }
void prepare_next() {}
"""

READ_WRITE = r"""
static bool read_lines(const char *, char *, size_t, char *, size_t) { return false; }
static bool write_lines(const char *, const char *, const char *) { return true; }
static bool spawn(const char *const *, bool preparing) {
    if (!spawn_ok) return false;
    ++launches;
    g_proc = &process;
    g_preparing = preparing;
    return true;
}
"""

DRIVER = r"""
int main(int argc, char **argv) {
    bool manual = !strcmp(argv[1], "manual");
    auto_check = !strcmp(argv[1], "automatic");
    lines = argv[2];
    exit_code = atoi(argv[3]);
    packaged = strcmp(argv[4], "unpackaged") != 0;
    spawn_ok = strcmp(argv[4], "spawn-failed") != 0;
    if (!strcmp(argv[4], "prepared")) g_state.phase = UPDATE_PREPARED;
    update_check_start(".", manual);
    if (g_proc) reader(nullptr);
    const char *phase = g_state.phase == UPDATE_IDLE ? "idle" :
                        g_state.phase == UPDATE_OFFER ? "offer" :
                        g_state.phase == UPDATE_CURRENT ? "current" :
                        g_state.phase == UPDATE_PREPARED ? "prepared" : "failed";
    printf("%d %s %s\n", launches, phase, g_state.error);
}
"""


@pytest.fixture(scope="module")
def check_probe(tmp_path_factory):
    cxx = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        pytest.skip("C++ compiler needed")
    path = tmp_path_factory.mktemp("update-check")
    launcher = ROOT / "runtime/ui/launcher"
    text = (launcher / "launcher_update.cpp").read_text(encoding="utf-8")

    def cut(start, end):
        offset = text.index(start)
        return text[offset:text.index(end, offset)]

    source, exe = path / "check.cpp", path / "check.exe"
    source.write_text(
        STUBS
        + cut("static SDL_Mutex *g_lock", "// The package's Python")
        + cut("static void ready_mark(", "static bool spawn(")
        + READ_WRITE
        + cut("static int SDLCALL reader(", "static bool spawn(")
        + cut("void update_check_start(", "void update_fetch_start(")
        + DRIVER,
        encoding="utf-8",
    )
    probe_build.build([cxx, "-I", str(launcher), str(source), "-o", str(exe)])
    return exe


@pytest.mark.parametrize(("mode", "line", "code", "scenario", "expected"), [
    ("disabled", "latest\n", 0, "normal", "0 idle"),
    ("automatic", "latest\n", 0, "normal", "1 idle"),
    ("automatic", "error check offline\n", 1, "normal", "1 idle"),
    ("manual", "latest\n", 0, "normal", "1 current"),
    ("manual", "update v0.1.16 100\n", 0, "normal", "1 offer"),
    ("manual", "error check offline\n", 1, "normal", "1 failed offline"),
    ("manual", "", 1, "normal", "1 failed the update check stopped"),
    ("manual", "", 0, "spawn-failed", "0 failed could not spawn"),
    ("manual", "", 0, "unpackaged", "0 failed this installation cannot check for updates"),
    ("manual", "latest\n", 0, "prepared", "0 prepared"),
])
def test_check_result(check_probe, mode, line, code, scenario, expected):
    p = subprocess.run([str(check_probe), mode, line, str(code), scenario], check=True,
                       capture_output=True, text=True, timeout=30)
    assert p.stdout.strip() == expected
