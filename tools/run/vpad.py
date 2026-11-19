# /// script
# requires-python = ">=3.11"
# dependencies = ["evdev>=1.7"]
# ///
"""Virtual Xbox 360 gamepad (Linux uinput) that plays a list of presses, for testing the overlay menu with a pad.

Why: `ctl.sh press` injects into the game's input (input_script.c), not into what the menu reads (input_pad_read, the
real pad through SDL), so opening the menu with Minus+Plus or moving its cursor needs a pad the system sees.
SDL takes the device as a real Xbox 360 pad (its mapping: Back = Minus, Start = Plus, the hat = D-pad).
The pad exists from start to end: SDL needs a moment to open it (--settle) before the first press.

Usage: uv run tools/run/vpad.py [--settle S] STEP...
  STEP: BUTTON[+BUTTON...][:MS]  hold those buttons together MS ms (default 120), then release
        wait:MS                  do nothing MS ms (default 300)
  BUTTON: a b x y lb rb back start guide ls rs up down left right
Example: uv run tools/run/vpad.py back+start:600 wait:800 right wait:500 b
Needs write access to /dev/uinput (group input or an ACL).
"""

import argparse
import sys
import time

from evdev import AbsInfo, UInput
from evdev import ecodes as e

BUTTONS = {"a": e.BTN_A, "b": e.BTN_B, "x": e.BTN_X, "y": e.BTN_Y, "lb": e.BTN_TL, "rb": e.BTN_TR,
           "back": e.BTN_SELECT, "start": e.BTN_START, "guide": e.BTN_MODE, "ls": e.BTN_THUMBL, "rs": e.BTN_THUMBR}
HAT = {"left": (e.ABS_HAT0X, -1), "right": (e.ABS_HAT0X, 1), "up": (e.ABS_HAT0Y, -1), "down": (e.ABS_HAT0Y, 1)}
STICK = AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)
TRIGGER = AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)
HATINFO = AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)


def make_pad() -> UInput:
    caps = {e.EV_KEY: list(BUTTONS.values()),
            e.EV_ABS: [(e.ABS_X, STICK), (e.ABS_Y, STICK), (e.ABS_RX, STICK), (e.ABS_RY, STICK),
                       (e.ABS_Z, TRIGGER), (e.ABS_RZ, TRIGGER), (e.ABS_HAT0X, HATINFO), (e.ABS_HAT0Y, HATINFO)]}
    # Microsoft's ids: SDL's controller database maps this vendor/product without a custom mapping
    return UInput(caps, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E, version=0x110,
                  bustype=e.BUS_USB)


def parse(step: str) -> tuple[list[str], int]:
    names, _, ms = step.partition(":")
    parts = names.split("+")
    for n in parts:
        if n != "wait" and n not in BUTTONS and n not in HAT:
            raise SystemExit(f"vpad: unknown button {n!r} in {step!r}")
    return parts, int(ms) if ms else (300 if names == "wait" else 120)


def play(pad: UInput, parts: list[str], ms: int) -> None:
    if parts == ["wait"]:
        time.sleep(ms / 1000)
        return
    for value in (1, 0):
        for n in parts:
            if n in BUTTONS:
                pad.write(e.EV_KEY, BUTTONS[n], value)
            else:
                axis, direction = HAT[n]
                pad.write(e.EV_ABS, axis, direction * value)
        pad.syn()
        time.sleep(ms / 1000 if value else 0.12)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--settle", type=float, default=2.0, help="seconds for SDL to open the new pad (default 2)")
    ap.add_argument("steps", nargs="+")
    args = ap.parse_args()
    steps = [parse(s) for s in args.steps]
    with make_pad() as pad:
        time.sleep(args.settle)
        for parts, ms in steps:
            print(f"vpad: {'+'.join(parts)} {ms} ms", flush=True)
            play(pad, parts, ms)
        time.sleep(0.3)


if __name__ == "__main__":
    sys.exit(main())
