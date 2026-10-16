#!/usr/bin/env python3
"""Render OneBoard's screen to PNG frames, then encode a GIF/MP4 demo.

WHAT THIS IS (read before trusting a frame)
-------------------------------------------
Frames are rendered from the LIVE board: the tool list and active tool come from
`/status` + the store's `/` listing, slot values come from `/state`, and the draw
primitives come from the tool's own behavior JSON. The drawing rules are the
firmware's, mirrored from `src/vm.cpp` (R_CLEAR/R_TEXT/R_RECT/R_FILLRECT/R_LINE/
R_CIRCLE/R_BAR/R_GRAPH) and from `drawHomeScreen()`/`drawHomeButton()` in
`src/main.cpp`.

It is therefore the board's real screen content reproduced on the host - NOT a
photograph of the physical panel, and not a browser-canvas approximation. Font
metrics are approximations of TFT_eSPI's GLCD fonts, so glyph placement can be a
pixel or two off the panel.

Usage
-----
  # cycle every tool in the store, plus the home launcher (no finger needed)
  python3 tools/render_frames.py --tour --out docs/media

  # render from a capture log produced by tools/demo_drive.py --record
  python3 tools/render_frames.py --log docs/media/demo-capture.json --out docs/media

Outputs <out>/oneboard-demo.gif and <out>/oneboard-demo.mp4
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

from PIL import Image, ImageDraw, ImageFont

W, H = 240, 320

# --- TFT_eSPI colour constants -> RGB -------------------------------------
COLORS = {
    "black": (0, 0, 0), "white": (255, 255, 255), "red": (255, 0, 0),
    "green": (0, 255, 0), "blue": (0, 0, 255), "yellow": (255, 255, 0),
    "cyan": (0, 255, 255), "magenta": (255, 0, 255), "orange": (255, 140, 0),
    "navy": (0, 0, 128), "darkgrey": (128, 128, 128), "darkgray": (128, 128, 128),
    "grey": (160, 160, 160), "gray": (160, 160, 160),
}

# TFT_eSPI GLCD font heights, in px (font 1 = 8px, 2 = 16px, 4 = 26px, 6 = 48px).
FONT_PX = {1: 8, 2: 16, 3: 24, 4: 26, 5: 32, 6: 48, 7: 56}

# main.cpp home-screen geometry
HOME_LIST_Y = 44
HOME_ROW_H = 30
HOME_ROWS_VISIBLE = (H - HOME_LIST_Y) // HOME_ROW_H  # 9

_FONT_CACHE: dict[int, ImageFont.FreeTypeFont] = {}
_FONT_PATHS = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
]


def font(px: int) -> ImageFont.FreeTypeFont:
    if px not in _FONT_CACHE:
        for p in _FONT_PATHS:
            if os.path.isfile(p):
                _FONT_CACHE[px] = ImageFont.truetype(p, px)
                break
        else:
            _FONT_CACHE[px] = ImageFont.load_default()
    return _FONT_CACHE[px]


def get_json(url: str, timeout: int = 8):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.load(r)


# --------------------------------------------------------------------------
# Firmware draw primitives (mirrors src/vm.cpp)
# --------------------------------------------------------------------------
def r_clear(d: ImageDraw.ImageDraw, rp: dict, slots: dict) -> None:
    d.rectangle([0, 0, W - 1, H - 1], fill=col(rp, slots))


def r_text(d: ImageDraw.ImageDraw, rp: dict, slots: dict) -> None:
    x, y = val(rp.get("x"), slots, 0), val(rp.get("y"), slots, 0)
    f = int(val(rp.get("font"), slots, 2) or 2)
    f = min(max(f, 1), 7)
    txt = src_text(rp, slots)
    fnt = font(FONT_PX[f])
    fill = col(rp, slots)
    if rp.get("center"):
        # MC_DATUM: x is the horizontal centre, y the vertical centre
        d.text((x, y), txt, font=fnt, fill=fill, anchor="mm")
    else:
        # TL_DATUM: top-left of the text cell at (x, y)
        d.text((x, y), txt, font=fnt, fill=fill, anchor="la")


def _box(x, y, w, h):
    """TFT_eSPI draws nothing for a zero/degenerate box; PIL raises. Return
    None so the caller skips."""
    if w <= 0 or h <= 0:
        return None
    return [x, y, x + w - 1, y + h - 1]


def r_rect(d, rp, slots):
    x, y = val(rp.get("x"), slots, 0), val(rp.get("y"), slots, 0)
    box = _box(x, y, val(rp.get("w"), slots, 0), val(rp.get("h"), slots, 0))
    if box:
        d.rectangle(box, outline=col(rp, slots), width=1)


def r_fillrect(d, rp, slots):
    x, y = val(rp.get("x"), slots, 0), val(rp.get("y"), slots, 0)
    box = _box(x, y, val(rp.get("w"), slots, 0), val(rp.get("h"), slots, 0))
    if box:
        d.rectangle(box, fill=col(rp, slots))


def r_line(d, rp, slots):
    d.line([val(rp.get("x"), slots, 0), val(rp.get("y"), slots, 0),
            val(rp.get("w"), slots, 0), val(rp.get("h"), slots, 0)],
           fill=col(rp, slots), width=1)


def r_circle(d, rp, slots):
    x, y = val(rp.get("x"), slots, 0), val(rp.get("y"), slots, 0)
    w = val(rp.get("w"), slots, 0)
    r = w if w > 0 else 10
    d.ellipse([x - r, y - r, x + r, y + r], fill=col(rp, slots))


def r_bar(d, rp, slots):
    x, y = val(rp.get("x"), slots, 0), val(rp.get("y"), slots, 0)
    w, h = val(rp.get("w"), slots, 0), val(rp.get("h"), slots, 0)
    box = _box(x, y, w, h)
    if not box:
        return
    if w > 0:
        d.rectangle(box, fill=col(rp, slots))
    d.rectangle(box, outline=col(rp, slots), width=1)


def r_graph(d, rp, slots):
    """Frame only. The trace comes from the firmware's VM_GRAPH_SIZE ring
    buffer, which /state does not expose, so a freshly loaded graph shows an
    empty frame - exactly what the panel shows on load."""
    x, y = val(rp.get("x"), slots, 0), val(rp.get("y"), slots, 0)
    w, h = val(rp.get("w"), slots, 0), val(rp.get("h"), slots, 0)
    if w <= 0:
        x, w = 10, 220
    if h <= 0:
        y, h = 100, 180
    d.rectangle([x, y, x + w - 1, y + h - 1], outline=COLORS["darkgrey"], width=1)


RENDERERS = {"clear": r_clear, "text": r_text, "rect": r_rect,
             "fillrect": r_fillrect, "line": r_line, "circle": r_circle,
             "bar": r_bar, "graph": r_graph}


def col(rp: dict, slots: dict) -> tuple:
    c = rp.get("color")
    if isinstance(c, dict):
        c = slots.get(c.get("slot"), c.get("lit"))
    return COLORS.get(str(c), (255, 255, 255))


def val(v, slots: dict, default=0):
    """Resolve a render argument: {slot:name} -> live value, {lit:N} -> literal,
    bare value -> itself. Mirrors the firmware's slot/lit resolution."""
    if isinstance(v, dict):
        if "slot" in v:
            got = slots.get(v["slot"])
            return default if got is None else got
        if "lit" in v:
            return v["lit"]
        return default
    return default if v is None else v


def src_text(rp: dict, slots: dict) -> str:
    s = rp.get("src")
    if isinstance(s, dict) and "slot" in s:
        got = slots.get(s["slot"])
        return "" if got is None else str(got)
    if isinstance(s, dict) and "lit" in s:
        return str(s["lit"])
    return ""


def walk_render_ops(doc: dict) -> list:
    """Every render primitive the behavior declares, in declaration order -
    the same list the firmware compiles into its render table."""
    ops = []

    def walk(actions):
        for a in actions or []:
            if not isinstance(a, dict):
                continue
            if "render" in a:
                ops.append(a["render"])
            if a.get("if"):
                walk(a["if"].get("then"))
                walk(a["if"].get("else"))

    for ev in doc.get("events", []):
        for h in ev.get("handlers", []):
            walk(h.get("run"))
    return ops


def draw_home_glyph(d: ImageDraw.ImageDraw) -> None:
    """The persistent back-arrow, mirrored from drawHomeButton() in main.cpp.

    The firmware draws this AFTER every tool render (loop() calls drawHomeButton
    following vmRender), so a tool can never erase it. It is NOT drawn on the
    launcher: drawHomeScreen() fillScreens the display and never redraws it.
    """
    cx, cy = 0 + 6, 0 + 6
    d.rectangle([cx + 5, cy + 4, cx + 5 + 5, cy + 4 + 2], fill=COLORS["yellow"])
    d.polygon([(cx + 1, cy + 5), (cx + 8, cy + 1), (cx + 8, cy + 11)],
              fill=COLORS["yellow"])


def render_tool_screen(doc: dict, slots: dict) -> Image.Image:
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    for rp in walk_render_ops(doc):
        fn = RENDERERS.get(str(rp.get("type")))
        if fn:
            fn(d, rp, slots)
    draw_home_glyph(d)   # firmware redraws the home arrow on top of the tool
    return img


# --------------------------------------------------------------------------
# Home launcher (mirrors drawHomeScreen + drawHomeButton in src/main.cpp)
# --------------------------------------------------------------------------
def render_home_screen(tools: list, active: str) -> Image.Image:
    img = Image.new("RGB", (W, H), COLORS["black"])
    d = ImageDraw.Draw(img)

    d.text((6, 6), "OneBoard \u2014 pick a tool", font=font(FONT_PX[2]),
           fill=COLORS["yellow"], anchor="la")
    d.line([0, 26, W - 1, 26], fill=COLORS["darkgrey"], width=1)

    for i in range(HOME_ROWS_VISIBLE):
        if i >= len(tools):
            break
        name = tools[i]
        y = HOME_LIST_Y + i * HOME_ROW_H
        cur = (name == active)
        if cur:
            d.rectangle([4, y - 4, 4 + 232 - 1, y - 4 + HOME_ROW_H - 4 - 1],
                        fill=COLORS["navy"])
        d.text((12, y), name, font=font(FONT_PX[2]),
               fill=COLORS["cyan"] if cur else COLORS["white"], anchor="la")

    if len(tools) > HOME_ROWS_VISIBLE:
        d.polygon([(200, 290), (210, 300), (220, 290)], fill=COLORS["darkgrey"])

    d.text((6, 310), "tap a tool  |  <| home", font=font(FONT_PX[1]),
           fill=COLORS["darkgrey"], anchor="la")
    return img


# --------------------------------------------------------------------------
# Frame sources
# --------------------------------------------------------------------------
def store_tools(store_url: str) -> list:
    j = get_json(store_url.rstrip("/") + "/")
    return [f[:-5] if f.endswith(".json") else f for f in j.get("behaviors", [])]


def board_snapshot(board: str) -> dict:
    return {"status": get_json(f"http://{board}/status"),
            "state": get_json(f"http://{board}/state")}


# The behaviors compiled into the firmware itself (PROGMEM), per
# progmemBehavior()/seedDefaultBehaviors() in src/main.cpp. A board flashed from
# scratch has exactly these - the rest live only in the store, so a demo that
# claims to show "what the firmware comes with" must not include them.
BUILTINS = ["timer", "temp-logger", "reaction-game", "tap-counter"]


def frames_from_tour(board: str, builtins_only: bool = False) -> list:
    """Cycle the board through every tool in its store plus the launcher.

    Each screen is real: /load is sent to the board, then /status + /state are
    read back and the screen is rendered from those values. No touch injection is
    involved (the firmware exposes no such endpoint).

    IMPORTANT: `homeMode` only clears on a real tap of a tool row (see
    homeHandleTouch in src/main.cpp) - /load does not clear it. So while the
    board reports home:true, the panel is showing the LAUNCHER, and that is what
    gets rendered for that step. Once a finger has taken it out of home mode,
    each /load produces a real tool screen.
    """
    st = get_json(f"http://{board}/status")
    tools = store_tools(st["store_url"])
    if builtins_only:
        tools = [t for t in tools if t in BUILTINS]
        if st.get("tool") not in tools and tools:
            # The board is sitting on a store-only tool, so the launcher would
            # render with nothing highlighted - true, but a poor first frame and
            # not what a from-scratch board looks like. Load a builtin first.
            get_json(f"http://{board}/load?id={tools[0]}&home=1")
            time.sleep(0.5)
            st = get_json(f"http://{board}/status") or st
    frames = []
    if st.get("home"):
        print("  ! board is in home mode - the panel is on the launcher. "
              "Tap a tool row once to take it out of home, then re-run for real "
              "tool screens.", file=sys.stderr)

    frames.append(("home", render_home_screen(tools, st.get("tool", ""))))

    for t in tools:
        try:
            # home=0 takes the board out of the launcher so the loaded tool is
            # what's actually on the panel (firmware support added 2026-10-16).
            get_json(f"http://{board}/load?id={t}&home=0")
        except Exception as e:  # noqa: BLE001
            print(f"  ! load {t} failed: {e}", file=sys.stderr)
            continue
        status = get_json(f"http://{board}/status") or {}
        if status.get("home"):
            # truthful: the panel still shows the launcher
            print(f"  ! {t}: still in home mode (firmware without home=0?) - "
                  "rendering the launcher instead", file=sys.stderr)
            frames.append(("home", render_home_screen(tools, t)))
            continue
        state = get_json(f"http://{board}/state")
        doc = get_json(st["store_url"].rstrip("/") + f"/{t}.json")
        slots = {s["name"]: s["value"] for s in state.get("slots", [])}
        frames.append((t, render_tool_screen(doc, slots)))

        # Put the board BACK on the launcher before rendering a launcher frame,
        # so every frame in the sequence is a real board state - not a screen we
        # drew while the panel was showing something else.
        try:
            get_json(f"http://{board}/load?id={t}&home=1")
            hstatus = get_json(f"http://{board}/status") or {}
            if hstatus.get("home"):
                frames.append(("home", render_home_screen(tools, t)))
            else:
                print(f"  ! {t}: home=1 did not open the launcher; "
                      "skipping the home frame", file=sys.stderr)
        except Exception as e:  # noqa: BLE001
            print(f"  ! home for {t} failed: {e}", file=sys.stderr)

    return frames


def frames_from_log(path: str) -> list:
    """Re-render frames captured by tools/demo_drive.py --record."""
    with open(path) as fh:
        cap = json.load(fh)
    frames = []
    for step in cap.get("steps", []):
        if step.get("screen") == "home":
            frames.append(("home", render_home_screen(step["tools"], step.get("active", ""))))
        else:
            slots = {s["name"]: s["value"] for s in step.get("slots", [])}
            frames.append((step.get("tool", "?"),
                           render_tool_screen(step["doc"], slots)))
    return frames


def encode(frames: list, out_dir: str, hold_ms: int, name: str) -> tuple:
    os.makedirs(out_dir, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="oneboard-frames-")
    paths = []
    for i, (_label, img) in enumerate(frames):
        p = os.path.join(tmp, f"f{i:04d}.png")
        img.save(p)
        paths.append(p)
    gif = os.path.join(out_dir, f"{name}.gif")
    mp4 = os.path.join(out_dir, f"{name}.mp4")

    fps = max(1, round(1000 / hold_ms))
    # GIF: per-frame palette keeps the flat TFT colours crisp
    vf = (f"fps={fps},scale=480:-1:flags=neighbor,"
          "split[a][b];[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=none")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "1",
                    "-i", os.path.join(tmp, "f%04d.png"), "-vf", vf,
                    "-loop", "0", gif], check=True)
    # MP4: 2x nearest-neighbour upscale so the panel pixels stay square
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error",
                    "-framerate", str(fps), "-i", os.path.join(tmp, "f%04d.png"),
                    "-vf", "scale=480:-1:flags=neighbor",
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18",
                    "-movflags", "+faststart", mp4], check=True)
    shutil.rmtree(tmp, ignore_errors=True)
    return gif, mp4


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="192.168.1.206")
    ap.add_argument("--log", help="capture log from demo_drive.py --record")
    ap.add_argument("--tour", action="store_true",
                    help="cycle every tool in the store (no finger required)")
    ap.add_argument("--builtins-only", action="store_true",
                    help="with --tour: only the four tools compiled into the "
                         "firmware, i.e. what a from-scratch board actually has")
    ap.add_argument("--out", default="docs/media")
    ap.add_argument("--hold-ms", type=int, default=1100)
    ap.add_argument("--name", default="oneboard-demo")
    ap.add_argument("--frames-dir", help="also dump the raw PNG frames here")
    a = ap.parse_args()

    if a.log:
        frames = frames_from_log(a.log)
    elif a.tour:
        frames = frames_from_tour(a.board, builtins_only=a.builtins_only)
    else:
        ap.error("pick --tour or --log")

    if a.frames_dir:
        os.makedirs(a.frames_dir, exist_ok=True)
        for i, (label, img) in enumerate(frames):
            img.save(os.path.join(a.frames_dir, f"{i:02d}-{label}.png"))

    gif, mp4 = encode(frames, a.out, a.hold_ms, a.name)
    print(f"{len(frames)} frames -> {gif}")
    print(f"{len(frames)} frames -> {mp4}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
