#!/usr/bin/env python3
"""Build the OneBoard hero video out of the phone footage in docs/media/.

    python3 tools/build_demo_cut.py                  # all segments -> docs/media/oneboard-demo-panel.mp4
    python3 tools/build_demo_cut.py --only 0,11,13   # re-render just those segments
    ONLY=0 python3 tools/build_demo_cut.py           # same, via env

The raw `VID_*.mp4` clips are untracked on purpose (they are phone 1080x1920 at
~30 Mbps). They live in docs/media/ locally and in Drive. The output MP4 is also
untracked: GitHub will not play an MP4 from a committed path, so the hero video is
attached to a release instead (see docs/DEMO-SHOTLIST.md).

Layout: 1920x1080 at 30 fps. The left column carries the step kicker, title and
caption; the right card holds the footage. The clips are portrait 1080x1920 with a
-90 rotation tag, so ffmpeg autorotates them and they are placed at 572x1016 --
shown as filmed, never cropped or stretched. Output is silent, deliberately: the
phone mic only ever recorded room noise and every platform autoplays muted.

Editing flow: change a caption or a segment's start/duration in SEGS, then re-render
only the touched segments (`--only i,j`) and the concat at the end re-stitches them.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "docs", "media")
OUT = os.path.join(SRC, "oneboard-demo-panel.mp4")
SEGDIR = os.path.join(os.environ.get("TMPDIR", tempfile.gettempdir()), "oneboard-cut")

W, H = 1920, 1080
BG = (11, 13, 16)
CARD = (18, 21, 26)
CARD_EDGE = (40, 46, 56)
ACCENT = (53, 208, 127)
WHITE = (238, 242, 246)
GREY = (154, 164, 178)

F_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
F_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

CARD_BOX = (1104, 16, 1904, 1064)
VID_XY = (1218, 32)
VID_WH = (572, 1016)

# kind, clip, start, duration, kicker, title, caption
# start/duration are source timestamps in the clip, verified by reading frames.
SEGS = [
    ("title", None, None, 3.5, None, "OneBoard",
     "One flash. Four tools. Nothing that arrived over WiFi."),
    ("clip", "VID_20261016_061810003.mp4", 3.6, 3.0, "01 / THE BOARD",
     "A bare CYD on the bench", "Erased. No tools, no credentials, no network."),
    ("clip", "VID_20261016_105756348.mp4", 13.6, 4.0, "02 / FLASH",
     "Erase, then flash", "pio run -t erase, then pio run -t upload. Done once."),
    ("clip", "VID_20261016_061810003.mp4", 24.0, 3.5, "03 / FIRST BOOT",
     "It runs on its own", "USB out. The firmware is the only thing on it."),
    ("clip", "VID_20261016_105913700.mp4", 20.0, 5.0, "04 / JOIN",
     "The board's own WiFi", "Open access point. No password, no app to install."),
    ("clip", "VID_20261016_110015793.mp4", 1.0, 5.5, "05 / SETUP PAGE",
     "It opens by itself", "Captive portal. Nothing typed, no URL entered."),
    ("clip", "VID_20261016_105913700.mp4", 25.5, 5.0, "06 / PROVISION",
     "Tell it your network", "It scans and lists 2.4 GHz networks with signal strength."),
    ("clip", "VID_20261016_110015793.mp4", 12.0, 6.0, "07 / JOINED",
     "It finds the store itself", "mDNS discovery -- there is no address to type."),
    ("clip", "VID_20261016_105756348.mp4", 43.5, 4.5, "08 / THE LAUNCHER",
     "Four tools", "timer / temp-logger / reaction-game / tap-counter"),
    ("clip", "VID_20261016_110141307.mp4", 23.5, 5.5, "09 / TIMER",
     "Timer", "Counts down on the panel. Touch to pause and resume."),
    ("clip", "VID_20261016_110141307.mp4", 0.5, 5.0, "10 / TEMP LOGGER",
     "Temp logger", "Live reading, with a graph the behavior declares."),
    ("clip", "VID_20261016_110141307.mp4", 18.6, 5.4, "11 / TAP COUNTER",
     "Tap counter", "Counts what the touch panel actually sees."),
    ("clip", "VID_20261016_061845332.mp4", 3.5, 4.0, "12 / CLOSE",
     "Four tools. One board.", "Flashed once. New tools arrive as behavior JSON, no reflash."),
    ("end", None, None, 4.0, None, "github.com/bradthurston/OneBoard",
     "ESP32-2432S028 / MIT / tools are behavior JSON, not firmware"),
]


def card_background(draw_card=True):
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    if draw_card:
        # accent hairline down the left edge of the text column
        d.rectangle((96, 96, 100, 984), fill=ACCENT)
        d.rounded_rectangle(CARD_BOX, radius=28, fill=CARD, outline=CARD_EDGE, width=2)
    return im, d


def wrap(draw, text, font, maxw):
    words, lines, cur = text.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if draw.textlength(t, font=font) <= maxw:
            cur = t
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def make_bg(seg, path):
    kind, clip, start, dur, kicker, title, caption = seg
    im, d = card_background(kind not in ("title", "end"))
    if kind in ("title", "end"):
        big = ImageFont.truetype(F_BOLD, 88 if kind == "title" else 60)
        sub = ImageFont.truetype(F_REG, 32)
        d.text(((W - d.textlength(title, font=big)) / 2, 470), title, font=big, fill=WHITE)
        for i, ln in enumerate(wrap(d, caption, sub, 1200)):
            d.text(((W - d.textlength(ln, font=sub)) / 2, 600 + i * 46), ln, font=sub, fill=GREY)
        d.rectangle((W / 2 - 60, 570, W / 2 + 60, 573), fill=ACCENT)
    else:
        d.text((140, 330), kicker, font=ImageFont.truetype(F_BOLD, 24), fill=ACCENT)
        y = 380
        f_title = ImageFont.truetype(F_BOLD, 62)
        for ln in wrap(d, title, f_title, 880):
            d.text((140, y), ln, font=f_title, fill=WHITE)
            y += 76
        y += 22
        f_cap = ImageFont.truetype(F_REG, 27)
        for ln in wrap(d, caption, f_cap, 880):
            d.text((140, y), ln, font=f_cap, fill=GREY)
            y += 40
    im.save(path)
    return path


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("FAILED:", " ".join(cmd[:6]), "...\n", r.stderr.strip()[-600:])
    return r.returncode == 0


def build_segment(i, seg, out):
    kind, clip, start, dur, *_ = seg
    bg = make_bg(seg, os.path.join(SEGDIR, f"bg_{i:02d}.png"))
    fade = ""
    if kind == "title":
        fade = ",fade=t=in:st=0:d=0.6"
    elif kind == "end":
        fade = f",fade=t=out:st={dur - 0.8:.2f}:d=0.8"
    base = ["ffmpeg", "-v", "error", "-y", "-loop", "1", "-t", f"{dur}", "-i", bg]
    tail = ["-r", "30", "-c:v", "libx264", "-preset", "medium", "-crf", "19", "-an", out]
    if kind in ("title", "end"):
        return run(base + ["-vf", f"fps=30,format=yuv420p{fade}"] + tail)
    vw, vh = VID_WH
    vx, vy = VID_XY
    fc = (f"[1:v]fps=30,scale={vw}:{vh},setsar=1,format=yuv420p{fade}[v];"
          f"[0:v][v]overlay={vx}:{vy}:shortest=1[o]")
    return run(base + ["-ss", f"{start}", "-t", f"{dur}", "-i", os.path.join(SRC, clip),
                       "-filter_complex", fc, "-map", "[o]"] + tail)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default=os.environ.get("ONLY"),
                    help="comma-separated segment indexes to re-render (default: all)")
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()
    todo = {int(x) for x in args.only.split(",")} if args.only else None

    os.makedirs(SEGDIR, exist_ok=True)
    parts, total = [], 0.0
    for i, seg in enumerate(SEGS):
        out = os.path.join(SEGDIR, f"seg_{i:02d}.ts")
        total += seg[3]
        if todo is not None and i not in todo:
            if not os.path.exists(out):
                sys.exit(f"segment {i} not built yet -- run without --only first")
            parts.append(out)
            continue
        if seg[1] and not os.path.exists(os.path.join(SRC, seg[1])):
            sys.exit(f"missing source clip {seg[1]}")
        if not build_segment(i, seg, out):
            sys.exit(1)
        parts.append(out)
        print(f"seg {i:02d} ok  {seg[0]:5s} {seg[3]:>4}s  {seg[5] or seg[4]}")

    lst = os.path.join(SEGDIR, "list.txt")
    with open(lst, "w") as fh:
        fh.write("".join(f"file '{p}'\n" for p in parts))
    ok = run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst,
              "-c", "copy", "-movflags", "+faststart", args.out])
    if not ok:  # TS -> MP4 stream copy can fail on odd containers; re-encode then
        ok = run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", lst,
                  "-c:v", "libx264", "-preset", "medium", "-crf", "19", "-pix_fmt",
                  "yuv420p", "-r", "30", "-movflags", "+faststart", args.out])
    if not ok:
        sys.exit("concat failed")
    print(f"concat ok -> {args.out}  (target {total:.1f}s, segments {len(parts)})")
    print(json.dumps({"segments": len(parts), "target_duration": round(total, 2)}))


if __name__ == "__main__":
    main()
