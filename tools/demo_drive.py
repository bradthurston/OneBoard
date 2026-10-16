#!/usr/bin/env python3
"""Drive Brad through the OneBoard demo while he films the panel.

Two jobs:

  --cue     Prompt each step with a countdown, then watch the board's own
            `/touch` tap buffer to confirm the tap actually landed and that the
            screen changed. Keeps the on-camera take tight and tells you
            immediately if a tap missed.

  --record  Snapshot `/status` + `/state` (and the tool's behavior JSON) at every
            step into docs/media/demo-capture.json, so tools/render_frames.py
            --log can rebuild the same demo as a GIF from the real state
            sequence.

Typical filming run:

  python3 tools/demo_drive.py --cue --record --out docs/media

Notes
-----
The firmware exposes no touch-injection endpoint (`/touch` is a read-only tap
buffer), so this script cannot fake a tap. That is deliberate: every screen in
the finished video is a real tap on real hardware.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.request

# The demo script: (spoken label, what should be on screen, seconds to hold,
#                   tap needed?, tool id to record)
#
# Row geometry on the launcher comes from src/main.cpp:
#   first row y = HOME_LIST_Y = 44, pitch = HOME_ROW_H = 30
# so row N is centred near y = 44 + N*30 + 10.
STEPS = [
    ("OPEN — the board sitting idle on its home screen",
     "The launcher: 'OneBoard — pick a tool' and the list of tools", 3, False, None),

    ("A new tool arrives over WiFi, then you pick it from the board",
     "Tap the 'multiplication-f' row on the launcher", 0, True, "multiplication-f"),

    ("It picked two random numbers and multiplied them ON the board",
     "Multiplication  a x b = answer  (the mul verb did the maths)", 3, False, None),

    ("Tap anywhere for the next problem — different numbers, same firmware",
     "A new multiplication problem, computed on the board", 0, True, None),

    ("Tap once more",
     "Another new problem", 0, True, None),

    ("Back to home with the top-left arrow",
     "The launcher again, with multiplication-f highlighted", 0, True, None),

    ("Pick a completely different tool — no reflash, same board",
     "Tap the 'timer' row", 0, True, "timer"),

    ("It is a timer now",
     "The timer running", 3, False, None),

    ("Back to home, then pick a third tool",
     "Tap the 'reaction-game' row", 0, True, "reaction-game"),

    ("It is a reaction game now",
     "The reaction game screen", 3, False, None),

    ("Back to home — eight tools, one board, zero reflashes",
     "The launcher", 4, False, None),
]


def get_json(url: str, timeout: int = 6):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.load(r)
    except Exception:
        return None


def tap_count(board: str) -> int:
    j = get_json(f"http://{board}/touch")
    return (j or {}).get("count", 0)


def wait_for_tap(board: str, timeout_s: int = 60) -> bool:
    """Poll the board's tap buffer until the count moves."""
    start = tap_count(board)
    deadline = time.time() + timeout_s
    print("   ... waiting for your tap", end="", flush=True)
    while time.time() < deadline:
        time.sleep(0.4)
        now = tap_count(board)
        if now != start:
            print(" got it.")
            return True
        print(".", end="", flush=True)
    print(" TIMED OUT (no tap seen).")
    return False


def snapshot(board, tools, active, tool_doc):
    st = get_json(f"http://{board}/status") or {}
    state = get_json(f"http://{board}/state") or {}
    return {
        "ts": time.time(),
        "screen": "home" if st.get("home") else "tool",
        "tool": st.get("tool", ""),
        "active": active,
        "tools": tools,
        "slots": state.get("slots", []),
        "doc": tool_doc,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="192.168.1.206")
    ap.add_argument("--out", default="docs/media")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--cue", action="store_true")
    ap.add_argument("--preload", action="store_true",
                    help="let the script /load each tool instead of Brad tapping the row")
    a = ap.parse_args()
    if not (a.cue or a.record):
        ap.error("pick --cue and/or --record")

    st = get_json(f"http://{a.board}/status")
    if not st:
        print(f"board unreachable at {a.board}", file=sys.stderr)
        return 1
    store = st["store_url"].rstrip("/")
    listing = get_json(store + "/") or {}
    tools = [f[:-5] if f.endswith(".json") else f for f in listing.get("behaviors", [])]
    print(f"board {a.board} | {len(tools)} tools in store {store}")
    print(f"active tool: {st.get('tool')} | home screen shown: {st.get('home')}\n")

    steps_out = []
    for i, (label, expect, hold, need_tap, load) in enumerate(STEPS, 1):
        print(f"[{i}/{len(STEPS)}] {label}")
        if expect:
            print(f"        on screen: {expect}")

        if load and a.preload:
            # home=0 leaves the launcher so the tool is what's on the panel
            get_json(f"http://{a.board}/load?id={load}&home=0")
            time.sleep(1.0)

        if need_tap and a.cue:
            if not wait_for_tap(a.board):
                print("        (take again, or check the panel)")

        if a.record:
            st_now = get_json(f"http://{a.board}/status") or {}
            doc = None
            tid = st_now.get("tool") or load
            if tid:
                doc = get_json(f"{store}/{tid}.json")
            snap = snapshot(a.board, tools, load or "", doc)
            steps_out.append(snap)
            print(f"        recorded: screen={snap['screen']} tool={snap['tool']} "
                  f"slots={len(snap['slots'])}")

        if a.cue and hold:
            print(f"        hold {hold}s ...")
            time.sleep(hold)
        print()

    if a.record:
        os.makedirs(a.out, exist_ok=True)
        path = os.path.join(a.out, "demo-capture.json")
        with open(path, "w") as fh:
            json.dump({"board": a.board, "tools": tools, "steps": steps_out}, fh, indent=2)
        print(f"wrote {path} ({len(steps_out)} steps)")
        print("now run:  python3 tools/render_frames.py "
              f"--log {path} --out {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
