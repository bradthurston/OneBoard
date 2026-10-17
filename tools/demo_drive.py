#!/usr/bin/env python3
"""Drive Brad through the OneBoard demo while he films the panel.

Two jobs:

  --cue     Prompt each step, tell you EXACTLY which launcher row to tap (row
            number + its y on the panel), then watch the board's own `/touch`
            buffer to confirm the tap landed and `/status` to confirm it opened
            the tool you were aiming at. A wrong row is caught on the spot
            instead of in the edit.

  --record  Snapshot `/status` + `/state` (and the tool's behavior JSON) at every
            step into docs/media/demo-capture.json, so tools/render_frames.py
            --log can rebuild the same demo as a GIF from the real state
            sequence. Consecutive duplicate states are dropped, so N frames ==
            N distinct screens.

  --auto-switch
            Let the script perform the tool SWITCHES itself via
            `/load?id=X&home=0`, so your only taps are the beats the video is
            actually about (pick the maths tool, tap for new problems, tap the
            back arrow).

Typical filming run:

  python3 tools/demo_drive.py --cue --record --out docs/media

Notes
-----
The firmware exposes no touch-injection endpoint (`/touch` is a read-only tap
buffer), so this script cannot fake a tap. That is deliberate: every screen in the
finished video is a real tap on real hardware.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.request

# main.cpp launcher geometry: first row's y, then the row pitch.
#   HOME_LIST_Y = 44, HOME_ROW_H = 30   ->  row i sits at y = 44 + i*30
HOME_LIST_Y = 44
HOME_ROW_H = 30


def row_y(tools: list, tool_id: str) -> tuple:
    """(row index, panel y of the row's text) for a tool, or (-1, -1)."""
    try:
        i = tools.index(tool_id)
    except ValueError:
        return (-1, -1)
    return (i, HOME_LIST_Y + i * HOME_ROW_H)


# (label, what should be on screen, hold seconds, tap needed, tool this step
#  should end up on - None = don't care)
STEPS = [
    ("OPEN - the board sitting idle on its home screen",
     "the launcher: 'OneBoard - pick a tool' + the tool list", 3, False, None),

    ("Pick the maths tool from the board itself",
     "tap the 'multiplication' row", 0, True, "multiplication"),

    ("It picked two random numbers and multiplied them ON the board",
     "Multiplication  a x b = answer   (the mul verb did the maths)", 3, False, "multiplication"),

    ("Tap anywhere for the next problem",
     "a different multiplication problem, computed on the board", 0, True, None),

    ("Once more",
     "another new problem", 0, True, None),

    ("Back to home with the top-left arrow",
     "the launcher again, multiplication highlighted", 0, True, None),

    ("Pick a completely different tool - no reflash, same board",
     "tap the 'timer' row", 0, True, "timer"),

    ("It is a timer now",
     "the timer screen", 3, False, "timer"),

    ("Back to home, then pick a third tool",
     "tap the 'reaction-game' row", 0, True, "reaction-game"),

    ("It is a reaction game now",
     "the reaction game screen", 3, False, "reaction-game"),

    ("Close on the launcher - eight tools, one board, zero reflashes",
     "the launcher", 4, False, None),
]


def get_json(url: str, timeout: int = 6):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.load(r)
    except Exception:
        return None


def status(board: str) -> dict:
    return get_json(f"http://{board}/status") or {}


def tap_count(board: str) -> int:
    return (get_json(f"http://{board}/touch") or {}).get("count", 0)


def wait_for_tap(board: str, timeout_s: int = 90) -> bool:
    start = tap_count(board)
    deadline = time.time() + timeout_s
    print("        waiting for your tap", end="", flush=True)
    while time.time() < deadline:
        time.sleep(0.35)
        if tap_count(board) != start:
            print(" - got it.")
            return True
        print(".", end="", flush=True)
    print(" - TIMED OUT, no tap seen.")
    return False


def state_key(snap: dict) -> str:
    """Identity of a screen: mode + tool + slot values. Lets the recorder drop
    duplicate consecutive frames so N frames == N distinct screens."""
    slots = tuple(sorted((s["name"], str(s["value"])) for s in snap.get("slots", [])))
    return f"{snap['screen']}|{snap['tool']}|{snap['active']}|{slots}"


def snapshot(board: str, tools: list, active: str) -> dict:
    st = status(board)
    state = get_json(f"http://{board}/state") or {}
    tid = st.get("tool", "")
    doc = None
    if tid:
        doc = get_json(f"{st.get('store_url', '').rstrip('/')}/{tid}.json")
    return {
        "ts": time.time(),
        "screen": "home" if st.get("home") else "tool",
        "tool": tid,
        "active": active,
        "tools": tools,
        "slots": state.get("slots", []),
        "doc": doc,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="192.168.1.206")
    ap.add_argument("--out", default="docs/media")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--cue", action="store_true")
    ap.add_argument("--auto-switch", action="store_true",
                    help="script performs the tool switches via /load?home=0")
    a = ap.parse_args()
    if not (a.cue or a.record):
        ap.error("pick --cue and/or --record")

    st = status(a.board)
    if not st:
        print(f"board unreachable at {a.board}", file=sys.stderr)
        return 1
    store = st["store_url"].rstrip("/")
    tools = [f[:-5] if f.endswith(".json") else f for f in
             (get_json(store + "/") or {}).get("behaviors", [])]
    print(f"board {a.board} | {len(tools)} tools | store {store}")
    print(f"active tool: {st.get('tool')} | launcher shown: {st.get('home')}\n")

    steps_out = []
    last_key = None
    for i, (label, expect, hold, need_tap, want_tool) in enumerate(STEPS, 1):
        print(f"[{i}/{len(STEPS)}] {label}")
        if expect:
            print(f"        on screen: {expect}")

        switching = bool(want_tool) and want_tool != status(a.board).get("tool")

        if switching:
            idx, y = row_y(tools, want_tool)
            if a.auto_switch:
                get_json(f"http://{a.board}/load?id={want_tool}&home=0")
                time.sleep(1.0)
                print(f"        (script loaded {want_tool})")
            elif idx >= 0:
                print(f"        >>> tap row {idx + 1}: '{want_tool}' "
                      f"(its text sits at about y={y} on the panel)")

        if need_tap and a.cue:
            if not wait_for_tap(a.board):
                print("        (missed tap - retake this beat)")
                continue
            if want_tool and not a.auto_switch:
                now = status(a.board).get("tool")
                if now != want_tool:
                    idx, y = row_y(tools, want_tool)
                    print(f"        !! that opened '{now}', not '{want_tool}'. "
                          f"you want row {idx + 1} (~y={y}).")
                    for _ in range(2):
                        if not wait_for_tap(a.board, 45):
                            break
                        now = status(a.board).get("tool")
                        if now == want_tool:
                            print(f"        now on '{want_tool}'.")
                            break
                        print(f"        !! still '{now}'.")

        if a.record:
            snap = snapshot(a.board, tools, want_tool or "")
            k = state_key(snap)
            if k == last_key:
                print("        (same screen as the last frame - not recorded)")
            else:
                steps_out.append(snap)
                last_key = k
                print(f"        recorded: {snap['screen']} / {snap['tool']} "
                      f"({len(snap['slots'])} slots)")

        if a.cue and hold:
            print(f"        hold {hold}s ...")
            time.sleep(hold)
        print()

    if a.record:
        # Deterministic close: put the board back on the launcher via the API
        # rather than asking for one more aimed tap.
        tid = status(a.board).get("tool", "")
        get_json(f"http://{a.board}/load?id={tid}&home=1")
        time.sleep(0.6)
        snap = snapshot(a.board, tools, tid)
        if state_key(snap) != last_key:
            steps_out.append(snap)
            print(f"final launcher frame recorded ({snap['screen']} / {snap['tool']})\n")

        os.makedirs(a.out, exist_ok=True)
        path = os.path.join(a.out, "demo-capture.json")
        with open(path, "w") as fh:
            json.dump({"board": a.board, "tools": tools, "steps": steps_out}, fh, indent=2)
        print(f"wrote {path} - {len(steps_out)} distinct screens")
        print("now run:  python3 tools/render_frames.py "
              f"--log {path} --out {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
