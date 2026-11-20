"""Bounded native Windows runs with reproducible settings and captured logs.

Why: race.sh relies on Linux's perf, FIFO and shell tools. This drives the same time-trial/phase hooks on Windows,
without replacing the installed executable or changing mkw.ini. Run from the repository root:
  uv run tools/run/windows.py pace-ab --exe build/dist/windows/mkw.exe --install D:/Emulation/mkw-recomp \
    --why "Compare acquire timers in one race" --env MKW_ACQUIRE_HIRES=ab --env MKW_AB=6
  uv run tools/run/windows.py selftest --exe D:/Emulation/mkw-recomp/mkw.exe --mode selftest
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("name", help="Log name (letters, digits, hyphens and underscores)")
    parser.add_argument("--exe", type=Path, default=Path("build/dist/windows/mkw.exe"))
    parser.add_argument("--install", type=Path, help="Prepared game folder; defaults to the executable's folder")
    parser.add_argument("--mode", default="race", choices=("race", "selftest", "probe-window", "probe-vkexec"))
    parser.add_argument("--seconds", type=int, default=48, help="Race measurement window")
    parser.add_argument("--timeout", type=int, default=240, help="Wall-clock safety limit")
    parser.add_argument("--why", help="Expected outcome and how it will be interpreted (required for a race)")
    parser.add_argument("--env", action="append", default=[], metavar="MKW_KEY=VALUE")
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("Use race.sh on Linux; this runner is for native Windows")
    if not args.name or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for c in args.name):
        parser.error("name must contain only letters, digits, hyphens and underscores")
    if args.seconds <= 0 or args.timeout <= 0:
        parser.error("seconds and timeout must be positive")
    if args.mode == "race" and not args.why:
        parser.error("--why must state the expected race outcome")
    exe = args.exe.resolve(strict=True)
    install = args.install.resolve(strict=True) if args.install else exe.parent
    if not install.is_dir():
        parser.error("--install must be a directory")
    changes = {"MKW_GAME_DIR": str(install)}
    if args.mode == "race":
        changes.update({
            "MKW_QUICK_RACE": "tt", "MKW_SEED": "1", "MKW_SEQ_WATCH": "1",
            "MKW_SEQ_UNTIL": "ReadyRace", "MKW_SEQ_EXIT": str(args.seconds), "MKW_TURBO": "until",
            "MKW_SKIP_MOVIES": "1", "MKW_TRACE": "perf,present", "MKW_GPU_GUARD": "1",
            "MKW_BACKGROUND_WINDOW": "1", "MKW_ASSUME_FOCUS": "1",
        })
    for entry in args.env:
        key, sep, value = entry.partition("=")
        if not sep or not key.startswith("MKW_") or not key[4:] or not key.replace("_", "").isalnum():
            parser.error("--env must be MKW_KEY=VALUE")
        changes[key] = value
    env = os.environ.copy()
    env.update(changes)
    # Probe the runtime's existing byte lock before a race. Its duplicate-instance message box would otherwise
    # consume the entire unattended timeout. The runtime still takes the real lock, closing any launch-time race.
    if args.mode == "race":
        import msvcrt

        profile = Path(env.get("MKW_PROFILE_DIR", "profile"))
        if not profile.is_absolute():
            profile = install / profile
        lock = profile / "instance.lock"
        if lock.exists():
            try:
                with lock.open("r+b") as stream:
                    msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
                    msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            except OSError as error:
                parser.error(f"Cannot acquire the game's profile lock ({lock}): {error}")
    out = Path("build/run").resolve()
    out.mkdir(parents=True, exist_ok=True)
    log_path = out / f"{args.name}.log"
    manifest_path = out / f"{args.name}.json"
    if log_path.exists() or manifest_path.exists():
        parser.error(f"Run {args.name!r} already exists; use a new name to preserve the evidence")
    command = [str(exe)]
    command += ["--seconds", str(args.seconds + 30)] if args.mode == "race" else [f"--{args.mode}"]
    manifest = {"command": command, "cwd": str(install), "why": args.why,
                "environment": {k: v for k, v in env.items() if k.startswith("MKW_")}, "timeout": args.timeout}
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    start = time.monotonic()
    timed_out = False
    with log_path.open("wb") as log:
        proc = subprocess.Popen(command, cwd=install, env=env, stdout=log, stderr=subprocess.STDOUT)
        print(f"PID {proc.pid}; log {log_path}", flush=True)
        try:
            code = proc.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            proc.wait()
            code = 124
    manifest.update({"exit_code": code, "timed_out": timed_out, "elapsed_seconds": time.monotonic() - start})
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Exit {code}; elapsed {manifest['elapsed_seconds']:.1f} s; log {log_path}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
