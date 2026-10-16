# OneBoard demo — filming kit

Two artefacts come out of one take:

1. **The hero video** — your phone filming the actual panel. This is the one that
   goes at the top of the README, because a hardware project that shows real
   hardware is the whole point.
2. **A GIF** — regenerated from the *same* tap sequence via
   `tools/demo_drive.py --record` → `tools/render_frames.py --log`. Same demo,
   same values, captured off the wire instead of off the camera.

## Before you roll

- Board powered, on WiFi, sitting on the home launcher.
- `python3 store/serve.py` reachable (the panel's store URL must answer) —
  the launcher's tool list comes from the store.
- Confirm the board answers: `curl -s http://192.168.1.206/status`
- Phone: **1080p at 30fps**, lock exposure and focus on the panel (tap-and-hold
  on the screen in the camera app). Auto-exposure hunting is the #1 way these
  clips look amateur.
- **Kill the room lights' flicker** — shoot at 1/60 or 1/50 shutter if your
  camera app exposes it; otherwise expect banding on the LCD.
- Angle: **slightly off-axis, ~15°**, so the panel fills the frame but you still
  see it's a board on a bench. Straight-on looks like a screenshot; too oblique
  loses the pixels.
- Phone steady on something solid. No handheld for the close-ups.
- Total target: **20–30 seconds**. GitHub viewers bounce; the GIF is the version
  people actually watch.

## Bringing the board up on cue

Run this in a terminal where you can see it while filming:

```bash
cd ~/projects/oneboard
python3 tools/demo_drive.py --cue --record --out docs/media
```

It prints each step, tells you what *should* be on the panel, waits for your tap,
and confirms the tap landed by watching the board's own `/touch` buffer — so a
missed tap gets caught on the spot instead of in the edit. When it finishes it
writes `docs/media/demo-capture.json`.

Then build the GIF from the same take:

```bash
python3 tools/render_frames.py --log docs/media/demo-capture.json --out docs/media
# -> docs/media/oneboard-demo.gif  and  oneboard-demo.mp4
```

## Shot list

| # | Shot | What you do | What the audience sees |
|---|------|-------------|------------------------|
| 1 | **Establishing** (3s) | Board idle on the home launcher, nothing touched | "OneBoard — pick a tool" + the tool list. It's a real board. |
| 2 | **Pick a tool** (2s) | Tap the `multiplication-f` row | The launcher is the UI. No app, no phone, no computer. |
| 3 | **It computes** (3s) | Let it sit | Two random numbers and their product, worked out **on the board** by the `mul` verb. Hold long enough to read. |
| 4 | **New problem** (2s) | Tap the screen | Different numbers, same firmware. Proves it's live, not a slide. |
| 5 | **New problem again** (2s) | Tap once more | Third set of numbers. |
| 6 | **Back home** (2s) | Tap the top-left back arrow | Returns to the launcher. |
| 7 | **Second tool** (3s) | Tap `timer` | Same board, now a stopwatch/timer. **No reflash.** |
| 8 | **Third tool** (3s) | Back-arrow → tap `reaction-game` | Now a game. |
| 9 | **Close** (3s) | Back-arrow, hold on the launcher | Eight tools, one board, zero reflashes. |

Optional B-roll if you want a longer video:

- Insert the USB cable still plugged in / the board still on its bench, to make
  the "no reflash" claim visually obvious.
- A quick cut of the terminal showing `curl -s http://192.168.1.206/schema`
  returning the verb list — "the board describes itself, any client can render it".

## After the take

```bash
# hero video -> keep next to the GIF
mv ~/Downloads/oneboard-*.mp4 docs/media/oneboard-demo-panel.mp4

# regenerate the GIF from the recorded tap sequence
python3 tools/render_frames.py --log docs/media/demo-capture.json --out docs/media
```

Commit `docs/media/oneboard-demo.gif` and reference it from the README. GitHub
renders GIFs inline from the repo, but **not** MP4 — for the hero video, attach
it to a release (`gh release upload vX.Y docs/media/oneboard-demo-panel.mp4`) and
link the release asset, or drag it into a GitHub issue/comment and paste the
generated `user-attachments` URL.

## Honest labelling

The GIF is rendered from the board's live `/status` + `/state` and the tool's own
behavior JSON, using the firmware's draw rules (mirrored from `src/vm.cpp` and
`drawHomeScreen()` in `src/main.cpp`). It is the board's real screen content —
**not** a photograph of the panel, and not a browser-canvas approximation. Font
metrics are host approximations of TFT_eSPI's GLCD fonts, so glyphs can sit a
pixel or two off the panel. Say so if anyone asks; don't imply it's a capture of
the LCD.

## Driving the board without a finger

`/load` now takes an optional `home` argument, so a script can put a tool on the
panel without anyone touching it:

```
/load?id=multiplication-f&home=0    # leave the launcher, show the tool
/load?id=timer&home=1               # open the launcher, tool paused
/load?id=dice                       # no arg: leave the mode alone (old behaviour)
```

`tools/render_frames.py --tour` uses `home=0` to walk every tool in the store.
The response now echoes the resulting mode: `{"ok":true,"tool":"dice","home":false}`.

## Known cosmetic issue visible in the demo

One pre-existing, cosmetic, non-functional issue — worth fixing before the video
goes up if you care:

1. **`temp-logger`'s axis labels were a lie.** It printed static `90F`/`60F`
   labels while `R_GRAPH` auto-scales Y to whatever is in its ring buffer — so the
   labels described an axis that didn't exist. They also sat at x=215, *inside*
   the graph frame (10..230), so the trace drew straight through them. **Fixed:**
   both labels removed. The graph is auto-scaled and the live reading is already
   printed above it, so the frame is self-explanatory.

`temp-logger.json` is served from `behaviors/`, so the store picks up the fix
immediately — no reflash needed for that one. Save it from the board's web page or
`/load` it again to see it.
