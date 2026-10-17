# OneBoard demo — the from-scratch take

**The story:** a bare board, nothing on it, to a working multi-tool device. Erase,
flash, first boot, captive portal from a phone, then the tools. The claim the video
makes is *this is what the firmware comes with* — so it must not show anything that
arrived over the network.

## The one rule for this shoot

**A plain upload is not a from-scratch board.** `pio run -t upload` leaves NVS and
LittleFS intact, so the board keeps its saved WiFi and anything cached in LittleFS. For a
genuinely fresh board you must **erase the flash first**. That also gets you:

- A board that comes up in provisioning mode with an **open** access point (no password).
- `seedDefaultBehaviors()` re-writing the four builtins into LittleFS on first boot.
- No saved WiFi, so `loadStoreToolList()` is skipped entirely (`wifiStationOk` is false) and
  the launcher shows only the builtins.

One thing that changed and matters here: the board now **finds the store by itself** over
mDNS, so "no `storeUrl`" no longer means "no store tools" — it finds the store as soon as
it joins WiFi. What keeps the caption honest is that the store itself now ships **only the
four builtins** (`examples/store-only/` is not served), so even once the board is on WiFi
the launcher lists the same four rows. Purging LittleFS matters too: the launcher adds any
cached behavior the store doesn't list, so a leftover cache copy of a deleted tool would
show up as a row.

If you film without erasing, the video contradicts its own caption. Before you roll, the
launcher must list exactly these four, in this set (store order can vary):

```
reaction-game
tap-counter
temp-logger
timer
```

Those are compiled into the firmware (`progmemBehavior()` / `seedDefaultBehaviors()` in
`src/main.cpp`), and they are also exactly what the store serves. The extras that used to
be store-only — `multiplication-f`, `dice`, `stopwatch`, `two-buttons` — now live in
`examples/store-only/`, which `serve.py` does not serve, so they cannot reach the launcher
at all.

## Cast

- **Phone A** — the camera. On a stand, framing the panel, panel filling most of the frame
  at a slight angle (~15°) so it still reads as a board on a bench rather than a screenshot.
- **Phone B** — the phone that *joins the board's AP* and shows the captive portal on
  camera. This is why you need two: the camera can't also be the device joining the AP.
- **Board + USB cable** — into the machine for the erase/flash, then unplugged so the
  footage shows it running standalone.
- Optional: the laptop in frame for the flash itself.

Camera settings that matter: **1080p30**, exposure and focus **locked** by tapping and
holding on the panel (auto-exposure hunting is the #1 way these clips look amateur),
shutter at 1/60 or 1/50 to avoid banding on the LCD, phone on something solid.

## Pre-flight

```bash
cd ~/projects/oneboard

# 1. Nothing half-finished in the tree
git status --short            # expect clean

# 2. Firmware builtins match the store copies. A fix applied only to
#    behaviors/*.json does NOT reach a freshly-flashed board — the board loads
#    the PROGMEM copy. This has already bitten once (temp-logger's axis labels).
python3 tools/check_builtins.py        # expect: all builtins in sync

# 3. All four envs still build
for e in esp32-2432s028 esp32-2432s024 esp32-2432s032-ili9341 esp32-2432s032-st7789; do
  pio run -e "$e" | grep -E 'SUCCESS|FAILED' | tail -1
done
```

## Erase and flash (shots 1–3 of the video)

```bash
# Wipes NVS + LittleFS + the app. This is what makes it "from scratch".
pio run -e esp32-2432s028 -t erase

# Flash the blank-canvas firmware. Never flashed again after this.
pio run -e esp32-2432s028 -t upload
```

Then **unplug USB** and power the board from a phone charger or battery — the next shot's
whole point is that it runs on its own.

## Shot list

| # | Shot | What happens | Duration |
|---|------|--------------|----------|
| 1 | **The blank board** | Board dark / freshly erased on the bench, USB in. "This board has nothing on it." | 3s |
| 2 | **Flash** | Screen recording of the two commands, or just the terminal. Cut fast — nobody wants to watch a flash. | 4s |
| 3 | **Unplug + power** | Cable out, then power applied. Board boots. | 4s |
| 4 | **First boot message** | Panel shows `Join AP to set up:` / `OneBoard-Stage3` / `Open browser, page opens` / `http://192.168.4.1`. Hold long enough to read all four lines. | 5s |
| 5 | **Phone B joins the AP** | On camera: WiFi settings → `OneBoard-Stage3` (open, no password) → connect. | 5s |
| 6 | **Captive portal auto-opens** | The setup page pops up by itself — nothing typed, no URL. **This is the money shot.** | 4s |
| 7 | **Provision** | Home WiFi SSID + password, submit. Angle away or blur the password. | 5s |
| 8 | **It joins** | Panel switches to the tool; the page reports the WiFi IP. | 4s |
| 9 | **The launcher** | Top-left arrow → four tools listed. "That's what's on the board. Four tools, no network." | 5s |
| 10 | **Timer** | Tap `timer` → counts up. | 4s |
| 11 | **Temp logger** | Back arrow → `temp-logger` → live reading + graph. | 4s |
| 12 | **Reaction game** | Back arrow → `reaction-game` → play one round. | 5s |
| 13 | **Tap counter** | Back arrow → `tap-counter` → tap a few times, count rises. | 5s |
| 14 | **Close** | Back to the launcher. "Four tools. One board. Flashed once." | 4s |

Target: **~60 seconds** for the whole story. The README GIF only needs shots 9–14.

### Driving the take

Shots 1–8 are not scriptable — the board has no WiFi until shot 7. Shoot them by hand.

Shots 9–14 are. Once the board is back on your network:

```bash
python3 tools/demo_drive.py --cue --record --out docs/media
```

It prompts each step, tells you **which launcher row** to tap and where it sits on the
panel (`>>> tap row 4: 'timer' (its text sits at about y=164)`), waits for the tap, then
reads `/status` and re-prompts if you hit the wrong tool. Consecutive duplicate screens
are dropped, so N recorded frames == N distinct screens.

Prefer not to aim at rows? `--auto-switch` makes the script do the tool switches itself;
then your only taps are the beats the video is actually about.

## Building the GIF

From your take:

```bash
python3 tools/render_frames.py --log docs/media/demo-capture.json --out docs/media
```

Or with no take at all (the script drives the board, builtins only):

```bash
python3 tools/render_frames.py --tour --builtins-only --out docs/media --hold-ms 1100
```

`--builtins-only` filters to the four firmware tools — the same set a from-scratch board
has, and the same set the store now serves. Keep the flag anyway: without it the renderer
picks up whatever is in the store, so the GIF would silently grow a row if a tool were ever
added there.

Commit `docs/media/oneboard-demo.gif` (the README embeds it) and the capture JSON
**together**, and only when the GIF was genuinely rendered from that take. GitHub will not
play an MP4 from a committed path — attach the hero video to a release
(`gh release upload vX.Y docs/media/oneboard-demo-panel.mp4`) or drag it into an issue and
paste the `user-attachments` URL.

## Verifying a take before you cut it

A contact sheet per clip (ffmpeg 3 stills at 15/50/85% → tile with PIL) tells you what each
clip actually contains — read it with a vision model, don't trust the filename.

To prove or disprove panel flicker in footage, pull **consecutive** frames, not samples:
at 30 fps a 100 ms clear shows up as *blank on every third frame*. A 12-frame burst at
1/30 s spacing (tile 6x2, label each timestamp) is enough for a vision read to state which
frames are blank — and the timestamps make the period obvious (blank at ...,033/,133/,233/333 s
= a 100 ms tick). Sampling every second instead can miss a 61% duty cycle entirely, and the
flicker only exists while the tool is *running*: an idle 00:00 stopwatch is a clean panel.

Don't measure brightness over a fixed crop to find it — the board drifts in frame and a crop
that isn't pinned to the LCD reads the bench, not the panel. Eyeball the burst instead.

## Honest labelling — put this in the video description or a comment

The GIF is rendered from the board's live `/status` + `/state` and the tool's own behavior
JSON, using the firmware's draw rules (mirrored from `src/vm.cpp` and
`drawHomeScreen()`/`drawHomeButton()` in `src/main.cpp`). It is the board's real screen
content — **not** a photograph of the panel, and not a browser-canvas approximation. Font
metrics are host approximations of TFT_eSPI's GLCD fonts, so glyphs can sit a pixel or two
off the panel. Don't imply it's a capture of the LCD.

**Known gap: the graph trace.** `R_GRAPH` draws from a ring buffer held inside the VM and
nothing exposes that history over HTTP — `/state` reports slot *values*, not the sample
buffer. So a rendered `temp-logger` frame shows the graph frame and the live reading with
an empty plot, where the panel would have a trace. Same gap that makes
`tools/board_view.html` label its graph handler `approximated`, i.e. "any client can render
the board" is not yet literally true for graph tools. A small `/graph` endpoint would close
it. **For the video, film the panel for temp-logger rather than trusting the GIF frame.**

## Driving the board without a finger (reference)

```
/load?id=timer&home=0     # leave the launcher, show the tool
/load?id=timer&home=1     # open the launcher, tool paused
/load?id=timer            # no arg: leave the mode alone (old behaviour)
```

The response echoes the resulting mode: `{"ok":true,"tool":"timer","home":false}`. Without
this, `homeMode` only cleared on a real tap of a launcher row, so no script could put a tool
on the panel at all.

## Already fixed, so don't film around it

- **The first-boot message is unwatchable, and that is our bug (not yet fixed).** `setup()`
  draws `Join AP to set up:` and then calls `loadBehavior(activeToolId)` two lines later
  (`src/main.cpp` ~1591 then ~1604), which clears the panel and paints the default tool over
  the instructions. The screen exists for a few hundred milliseconds, so **shot 4 cannot be
  filmed at all** and a freshly erased board reads as "a board running a timer". Two shoots
  missed it before this was found. The fix (Brad's call, not applied): skip `loadBehavior`
  while `provisioningMode`, and in `handleWifi()` on a successful join paint a short
  `Joined <ssid> -- <ip>` line and *then* load the tool, so the panel answers when setup
  succeeds. Until that lands, a cut either omits shot 4 or says in the description that the
  first-boot screen is there for a moment before the tool loads.
- **The timer's hint line was being truncated by the VM, not by the layout.** `Touch to
  pause/resume` drew as `Touch to pause/resu`. The first write-up here blamed the 240 px panel
  and suggested shortening the string — that was wrong, and it would have treated the symptom.
  TFT_eSPI's own font-2 width table puts the string at **139 px**, i.e. comfortably inside 240.
  The real cause: `RenderOp.text` is `char[VM_STR_LEN]` and `vm.cpp` fills it with
  `strncpy(..., VM_STR_LEN - 1)`, so at `VM_STR_LEN 20` every literal lost its tail silently —
  no error, no warning, just a misspelled word on the panel. Fixed: `VM_STR_LEN` is now 40, the
  validator errors on a literal over the limit, and a regression test asserts a 21-char literal
  reaches the panel whole.
- **`temp-logger`'s axis labels** used to print static `90F`/`60F` while `R_GRAPH`
  auto-scales Y to its ring buffer — labels describing an axis that didn't exist — and they
  sat at x=215, *inside* the graph frame (10..230), so the trace drew through them. Removed
  from both the store copy and the PROGMEM builtin.

## What was actually shot and cut (Oct 16 2026)

Brad filmed three batches, 18 clips, into `docs/media/` — all portrait 1080x1920 with a −90
rotation tag (ffmpeg autorotates; `-vf crop` then works in rotated coordinates). Read
contact sheets before trusting timestamps: the two batches caught different things, and the
first batch alone looked like a failed erase.

Hero video: `docs/media/oneboard-demo-panel.mp4` — 1920x1080, 30 fps, **57.5 s**, 15
segments, silent, built by `tools/build_demo_cut.py` (re-run it after editing `SEGS`;
`--only i,j` re-renders single segments and the concat re-stitches). Source clips and the MP4
are both untracked; attach the MP4 to a release. A third batch (6 clips, 2026-10-17,
`…0353xx`/`…0354xx`) replaced every per-tool shot and added the live-change take; the earlier
63.6 s cut is kept beside it as `docs/media/oneboard-demo-panel-v1.mp4`.

The cut leads with the claim rather than a feature tour: the firmware is an interpreter
flashed once, and the tools are behaviour JSON the board fetches. The old cut spent 32% of
its runtime on a tool tour and stated the claim only in its last card, which is why the
per-tool shots are now 3 s each and the live change is the centrepiece.

| cut | shot | source clip | in-clip |
|---|---|---|---|
| 00 | title card ("One flash.") | — | — |
| 01 | the blank board | `…061810003` | 3.6 s |
| 02 | flash / esptool | `…105756348` | 13.6 s |
| 03 | boots and runs alone | `…061810003` | 24.0 s |
| 04 | join the board's AP | `…105913700` | 20.0 s |
| 05 | captive portal opens | `…110015793` | 1.0 s |
| 06 | provision | `…105913700` | 25.5 s |
| 07 | joined + store found | `…110015793` | 12.0 s |
| 08 | the tool list | `…105756348` | 43.5 s |
| 09 | **live change over WiFi** | `…035523677` | 6.0 s (11.0 s long) |
| 10 | timer | `…035448981` | 3.6 s |
| 11 | temp logger + graph | `…035434777` | 1.8 s |
| 12 | tap counter | `…035313276` | 2.2 s |
| 13 | reaction game | `…035416284` | 4.0 s |
| 14 | end card + repo | — | — |

Gaps, stated rather than faked:

- **Shot 4 (first-boot message)** — still unfilmable until the boot-order fix above lands.
- **Shot 12 (reaction-game)** — no longer a gap: filmed 2026-10-17 as `…035416284`, and the
  clip catches the result screen, the green READY wait and the red TAP! state.
- **The live change** (`…035523677`, 18.5 s, one unbroken take): the tool's JSON on the store
  was copied over at 10.5 s into the clip and the panel finished recompiling and redrawing at
  12.9 s. The cut uses 6.0-17.0 s, so the change lands about 4.5 s into the segment, and the
  count resetting to 0 is the visible proof that the tool re-read its file.
- **The Stopwatch is left out of the cut on purpose.** `behaviors/i-need-a-stopwat.json` is
  served by the store — its index reports 5 behaviours — and clip `…035330863` (9.4 s) is that
  tool. It is deliberately not used, and shot 08 is yesterday's footage that honestly shows
  four rows. Shot 08's caption therefore claims the list comes *from the store*, never a row
  count, so it stays true as the store's contents change.
- The `…102704981` clip (116 s, `/make` of a multiplication table) is **not** in the cut: it
  is a different story and it was filmed pre-flicker-fix.

The flicker fix is visible in the footage and worth keeping as evidence: 12 consecutive
frames at 1/30 s of the **running** timer in `…110141307` show **0 blank frames**, where the
same test on the pre-fix stopwatch in `…102704981` (t=110 s) showed **4 blank of 12, 100 ms
apart**. Pull consecutive frames, never samples, when checking this.
