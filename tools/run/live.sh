#!/usr/bin/env bash
# A game run that stays up for inspection through its control FIFO (runtime/core/base/ctl.c): screenshots, frame dumps,
# flight dumps and presses go to it with tools/run/ctl.sh, each in seconds, instead of a new run replaying the boot
# and the menus to reach the screen (60-90 s per look).
# Usage: tools/run/live.sh NAME [SECONDS]   (default 900; then the run is killed). Extra env (MKW_*) passes
# through; the defaults are those of the race runs (720p romfs overlay, 768 MiB of sparse tiles, BC1 off).
# Stop: tools/run/ctl.sh NAME stop. Log: build/run/NAME.log, FIFO: build/run/NAME.ctl.
# LIVE_WRAP: a command build/mkw runs under, e.g. an HDR display for a machine without one:
#   LIVE_WRAP="gamescope --backend headless -W 1280 -H 720 --hdr-enabled --hdr-debug-force-support --" (gamescope's
#   Vulkan layer on with ENABLE_GAMESCOPE_WSI=1, exported before live.sh).
# LIVE_BIN: another runtime than build/mkw, e.g. a prepared player folder of another game version
#   (LIVE_BIN=scratchpad/prep-linux-180/mkw: 1.8.0, whose game code build/mkw does not carry). A runtime from a release
#   AppImage links the sysroot's libvpx.so.6 and SDL3, which only the AppImage's lib/ has: LIVE_LIB=<that lib/> (e.g.
#   from `AppImage --appimage-extract 'lib/*'`) puts it first on the library path.
set -euo pipefail
cd "$(dirname "$0")/../.."
n=${1:?usage: live.sh NAME [SECONDS]}; secs=${2:-900}
. tools/run/why.sh "$n"
# Refuse a stale build/mkw: a runtime source newer than it means the last build did not run or failed (01/11/2026:
# three runs measured the previous binary after a commit hook stopped the build and a compile error went unseen).
bin=${LIVE_BIN:-build/mkw}
if [ -z "${LIVE_BIN:-}" ] && [ -n "$(find runtime -newer build/mkw \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) -print -quit)" ]; then
  echo "${0##*/}: runtime/ is newer than build/mkw: build first (tools/build/build.py)" >&2; exit 1
fi
if pgrep -x mkw >/dev/null; then echo "live: a build/mkw is already running (one run at a time)" >&2; exit 1; fi
# MIN_AVAIL only raises the floor: below 8500 the system OOM killed runs and the desktop (on an integrated GPU;
# 07/11/2026: four runs launched at 8000 to get past it).
[ "${MIN_AVAIL:-8500}" -ge 8500 ] || { echo "MIN_AVAIL=${MIN_AVAIL} refused: the floor is 8500 MB; wait for memory instead"; exit 1; }
tools/run/gpu_ok.sh || exit 1   # no run on a GPU left where the next fault can freeze the desktop
# The pre-submit checks stop the run before a fault reaches the GPU (vk_guard.c); MKW_GPU_GUARD=0 to measure without.
export MKW_GPU_GUARD=${MKW_GPU_GUARD:-1}
avail=$(tools/run/avail.sh --wait "${MIN_AVAIL:-8500}")   # WAIT_MEM=<min>: wait for it instead of refusing
[ "$avail" -ge "${MIN_AVAIL:-8500}" ] || { echo "live: only $avail MB available (with the GPU page pool): not starting"; exit 1; }
mkdir -p build/run
rm -f "build/run/$n.ctl"
mkfifo "build/run/$n.ctl"
export MKW_CTL=$PWD/build/run/$n.ctl MKW_ROMFS_PATCHES=${MKW_ROMFS_PATCHES-build/romfs-res}
export MKW_RESOLUTION=${MKW_RESOLUTION:-720}   # the overlay's lines for the game output too (patches.c)
# A run is never the start screen: a prepared player runtime (LIVE_BIN) opens it unless told otherwise (main.c
# want_launcher; 14/11/2026: seq180d sat on the start screen).
export MKW_NO_LAUNCHER=1
export MKW_SPARSE_MIB=${MKW_SPARSE_MIB:-768} MKW_GPU_MEM_LIMIT=${MKW_GPU_MEM_LIMIT:-4096}
export MKW_FLIGHT=${MKW_FLIGHT:-1} MKW_FLIGHT_SPIKE_MS=${MKW_FLIGHT_SPIKE_MS:-0} MKW_FLIGHT_DIR=${MKW_FLIGHT_DIR:-$PWD/scratchpad/flight-$n}
export MKW_TRACE=${MKW_TRACE:-gfx,present}
# With MKW_SEQ_UNTIL the way to that scene goes at turbo speed and the turbo goes off there (core/hacks/turbo.c).
[ -z "${MKW_SEQ_UNTIL:-}" ] || export MKW_TURBO=${MKW_TURBO:-until}
# Our runs open their window behind whatever is in use, without the focus; the game is told it has it (it pauses
# otherwise). MKW_BACKGROUND_WINDOW=0 brings the window to the front.
export MKW_BACKGROUND_WINDOW=${MKW_BACKGROUND_WINDOW:-1} MKW_ASSUME_FOCUS=${MKW_ASSUME_FOCUS:-1}
tools/run/kwin-rule.sh --ensure >/dev/null   # the rule that keeps that window minimized and without focus
# job start runs a systemd service, which does not inherit this shell's environment: the MKW_* go by env, and the
# SDL_* hints too (SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1: tools/run/vpad.py's pad reaches the unfocused window).
mapfile -t vars < <(env | grep -E '^(MKW_|SDL_|ENABLE_GAMESCOPE_WSI=)')
[ -n "${LIVE_LIB:-}" ] && vars+=("LD_LIBRARY_PATH=$(realpath "$LIVE_LIB")")
JOB_MEM=${JOB_MEM:-7G} tools/run/job.sh start "$n" env "${vars[@]}" bash -c "timeout -s KILL $secs ${LIVE_WRAP:-} $bin > build/run/$n.log 2>&1"
echo "live: $n up; commands: tools/run/ctl.sh $n frame|shot PATH|dump DIR [ubo]|flight|press <ticks> <button> [hold]|stop"
