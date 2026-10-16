# OneBoard demo — the from-scratch take

**The story:** a bare board, nothing on it, to a working multi-tool device. Erase,
flash, first boot, captive portal from a phone, then the tools. The claim the video
makes is *this is what the firmware comes with* — so it must not show anything that
arrived over the network.

## The one rule for this shoot

**A plain upload is not a from-scratch board.** `pio run -t upload` leaves NVS and
LittleFS intact, so the board keeps its saved WiFi and its cached store tools, and the
launcher lists **eight** tools instead of four. For a genuinely fresh board you must
**erase the flash first**. That also gets you:

- `storeUrl` falling back to the compiled default (`http://192.168.4.100:8080`), which is
  unreachable — so `loadStoreToolList()` finds no store tools and the launcher shows only
  the builtins.
- `seedDefaultBehaviors()` re-writing the four builtins into LittleFS on first boot.
- A board that comes up in provisioning mode with an **open** access point (no password).

If you film without erasing, the video contradicts its own caption. Before you roll, the
launcher must list exactly these four, in this set (store order can vary):

```
reaction-game
tap-counter
temp-logger
timer
```

Those are compiled into the firmware (`progmemBehavior()` / `seedDefaultBehaviors()` in
`src/main.cpp`). Everything else in `behaviors/` is store-only and does not exist on a
fresh board — including `multiplication-f`, `dice`, `stopwatch` and `two-buttons`.

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
has. Without it you get every tool in the store, which is not what this video claims.

Commit `docs/media/oneboard-demo.gif` (the README embeds it) and the capture JSON
**together**, and only when the GIF was genuinely rendered from that take. GitHub will not
play an MP4 from a committed path — attach the hero video to a release
(`gh release upload vX.Y docs/media/oneboard-demo-panel.mp4`) or drag it into an issue and
paste the `user-attachments` URL.

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

- **`temp-logger`'s axis labels** used to print static `90F`/`60F` while `R_GRAPH`
  auto-scales Y to its ring buffer — labels describing an axis that didn't exist — and they
  sat at x=215, *inside* the graph frame (10..230), so the trace drew through them. Removed
  from both the store copy and the PROGMEM builtin.
