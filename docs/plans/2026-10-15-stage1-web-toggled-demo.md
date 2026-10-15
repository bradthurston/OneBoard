# Stage 1 — Web-Toggled Demo Implementation Plan

> **For the worker:** Build this plan task-by-task. Validate each step actually compiles before moving on. You are a headless kanban worker on DeepSeek-V4-Flash. When done, call kanban_complete with the EXIT STATE format from your CONTRACT.

**Goal:** Flash the CYD board once with firmware that plays one of 3 fixed demo behaviors, switched live from a WiFi web page — proving the control + persistence + hardware-test loop with no interpreter yet. This is a controlled cheat on the way to a real behavior VM.

**Architecture:** The ESP32 runs its own soft-AP + HTTP web server. A browser connects to it and taps a button to switch which demo is active. The active demo renders on the 2.8" TFT. The board reboots into whichever demo was last selected (persistence via Preferences/EEPROM). Three simple demo behaviors: a count-up timer, a fake temperature logger (cycles a thermistor-style reading + history), and a reaction game (this is where touch input proves itself).

**Tech Stack:**
- Board: ESP32-2432S028 (vanilla) — ESP32-WROOM-32 classic (NOT S3), 2.8" TFT ILI9341, resistive touch XPT2046, WiFi/BT native, ~300KB usable SRAM, 4MB flash.
- Toolchain: PlatformIO CLI (`pio`), `espressif32` platform, Arduino framework.
- PlatformIO Board config: use an ESP32 devkit board def + custom `board_build.partitions` or pin map. Confirm exact CYD pin map below.
- Storage: Preferences (NVS) for last-selected demo.

---

## CYD 2432S028 Pin Reference (use these — they're the standard ESP32-2432S028 ILI9341 + XPT2046 map)

```
ILI9341 TFT:
  TFT_CS    = 15
  TFT_DC    = 2
  TFT_RST   = 4         (SD card CS shares 4 on some printings — mostly unused)
  TFT_BL    = 21        (backlight; drive HIGH)
  MOSI/SDA  = 23
  MISO/SDO  = 19
  SCK       = 18

XPT2046 Touch:
  TOUCH_CS  = 14
  TOUCH_IRQ = 27        (can be left as GPIO input in polling mode)
  (MOSI/MISO/SCK shared with TFT above)

Optional:
  SD_CS     = 5         (not used in Stage 1)
  Speaker   = 25        (passthrough resistor; optional beep — skip for Stage 1)
```

Layout note: this pin map matches the common ESP32-2432S028 schematic (TFT+Touch share the SPI bus, separate CS lines). If a compile/link constant name differs, keep the numbers — these are the board's actual pins.

---

## Task 1: Scaffold the PlatformIO project

**Objective:** Stand up a compiling bare-metal "hello" that proves the toolchain + board targets work before any LCD/touch code.

**Files:**
- Create: `/home/brad/projects/oneboard/platformio.ini`
- Create: `/home/brad/projects/oneboard/src/main.cpp` (blink via `LED_BUILTIN` or a GPIO toggle)

**Step 1: Write platformio.ini**

```ini
[env:esp32-2432s028]
platform = espressif32
board = upesy_wrover_kit        ; nearest ESP32-WROOM board def; we override pins in code
framework = arduino
monitor_speed = 115200
board_build.flash_size = 4MB
```

If `upesy_wrover_kit` doesn't exist in the installed platform, fall back to `esp32dev` (ESP32-WROOM-32 DevKit). The exact board def matters less than the chip being ESP32 classic; all pin control is in code.

**Step 2: Write a minimal blink main.cpp**

```cpp
#include <Arduino.h>
void setup()   { pinMode(2, OUTPUT); }
void loop()    { digitalWrite(2, !digitalRead(2)); delay(500); }
```

**Step 3: Compile**

Run: `cd /home/brad/projects/oneboard && export PATH=$HOME/.local/bin:$PATH && pio run`
Expected: `SUCCESS`, firmware binary produced under `.pio/build/<env>/firmware.bin`.

**Step 4: Done when the build passes.** No flash — Brad handles flashing.

---

## Task 2: Wire the TFT display (ILI9341)

**Objective:** Get the 2.8" screen rendering — the display of every demo.

**Files:**
- Create: `src/displays.h` / `src/displays.cpp` (thin wrapper; or keep inline in main.cpp if simpler)
- Modify: `src/main.cpp`
- Modify: `platformio.ini` (add library deps)

**Step 1:** Add dependencies to `platformio.ini`:

```ini
lib_deps =
    bodmer/TFT_eSPI@^2.5.0
```

**Step 2:** Configure TFT_eSPI pin map. Create `src/User_Setup.h` override (TFT_eSPI uses `User_Setup.h` for pin defines). Set pins per the pin reference above:

```cpp
#define USER_SETUP_LOADED
#define ILI9341_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BL   21
#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18
#define SPI_FREQUENCY  27000000
```

Ensure the file is discoverable — set `build_flags` or place `User_Setup.h` where TFT_eSPI finds it (common: put it beside the lib in `.pio/libdeps/<env>/TFT_eSPI/User_Setup.h`, or add a `-I` include path to your `src/`). A `lib_extra_dirs` or include-dir flag in `platformio.ini` is acceptable if that's more reliable.

**Step 3:** In `setup()`, init the display:

```cpp
#include <TFT_eSPI.h>
TFT_eSPI tft = TFT_eSPI();
// in setup:
pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH); // backlight on
tft.init(); tft.setRotation(1);   // landscape
tft.fillScreen(TFT_BLACK);
tft.setTextColor(TFT_WHITE, TFT_BLACK);
tft.drawString("Stage 1 ready", 10, 10, 4);
```

**Step 4:** Compile. Expected: `SUCCESS`. This proves the screen driver + pin map are right before the web layer exists.

---

## Task 3: Add a soft-AP web server with a toggle page

**Objective:** Board hosts its own WiFi network + a tiny HTTP page with 3 buttons that switch the active demo.

**Files:**
- Modify: `src/main.cpp`
- Create: `src/web.h` / `src/web.cpp` (optional separation — keep it simple, inline is fine)
- Create: `src/demos.h` defining the 3 demo behaviors

**Step 1:** In `setup()`, after display init, start the AP and HTTP server:

```cpp
#include <WiFi.h>
#include <WebServer.h>
WebServer server(80);
const char* ssid = "OneBoard-Stage1";   // no password on this LAN-locked demo
// in setup:
WiFi.softAP(ssid);
// route handlers below
server.begin();
```

**Step 2:** HTTP routes:
- `GET /` → serve the control page (HTML with 3 buttons: Timer, Temp Logger, Reaction Game).
- `POST /demo/<id>` or `GET /set?demo=timer|temp|reaction` → set the active demo, store to NVS, return `{"ok":true,"demo":"<id>"}`.

Keep the HTML dead simple — no frameworks, inline `<style>`/`<script>`, buttons that hit the set endpoint and show the returned id. This is a demo page, not a product.

**Step 3:** NVS persistence:

```cpp
#include <Preferences.h>
Preferences prefs;
const char* DEMO_NS = "obm_demo";
// save on set:
prefs.begin(DEMO_NS, false); prefs.putString("active", id); prefs.end();
// load at boot:
prefs.begin(DEMO_NS, true); String active = prefs.getString("active", "timer"); prefs.end();
```

**Step 4:** Compile. Expected: `SUCCESS`.

---

## Task 4: Implement the three demo behaviors, switchable at runtime

**Objective:** Three render loops, each draws on the TFT, only the active one runs.

**Files:**
- Create: `src/demos.h` and `src/demos.cpp` with an enum + dispatch, OR keep three functions in main.cpp. Prefer a small clean dispatch.
- Modify: `src/main.cpp`

**Step 1:** Define the demo enum + a `setActiveDemo(String id)` that updates the active selector (and NVS). `loop()` calls only the active demo's tick function.

**Step 2 — Timer demo:** big MM:SS count-up on the screen, redraws every second (or 100ms for seconds precision). Include a large "00:00" that ticks. Optional: draw a simple progress bar.

**Step 3 — Temp logger demo:** simulate a temperature sensor (sin wave or random walk around 72°F), draw current temp + a rolling line-graph of the last N readings. This proves the screen handles continuous draws.

**Step 4 — Reaction game (touch test):** shows "TAP!" then times how fast touch is registered. This is the touch-input proof. Steps within the demo:
  1. Screen shows a green "READY..." circle.
  2. After a random 1-5s delay, it flips to red "TAP!" and starts a millis() timer.
  3. On first detected touch (`getTouch()` from XPT2046 via TFT_eSPI's `getTouch(&px,&py)`), it prints the reaction time in ms.
  4. Debounce touch; ignore touches during the waiting phase so "anticipation" doesn't count.

**Step 5 — Runtime switching:** the web set-endpoint calls `setActiveDemo`, which causes `loop()` to swap to the new demo on the next tick. Each demo must leave the screen in a clean re-drawable state (call `tft.fillScreen` on entry).

**Step 6:** Compile. Expected: `SUCCESS`.

---

## Task 5: Verify & hand off

**Objective:** Prove the whole thing compiles, the binary exists, and give Brad exact flash instructions.

**Steps:**
1. Final `pio run` — must be `SUCCESS`, no errors.
2. Confirm the flashable binary exists: report absolute path to `.pio/build/<env>/firmware.bin` (list what env folders exist).
3. Report the soft-AP SSID (`OneBoard-Stage1`) and the URL to open (`http://192.168.4.1/`).
4. Note for Brad: connect to the AP, open the page, tap the 3 demo buttons, confirm the screen switches and the selection survives a reboot.
5. If the TFT/touch pin map causes issues, correct the numbers to the standard 2432S028 map above — do NOT guess a different board.

---

## Acceptance criteria (all must hold for the card to be done)

- [ ] `pio run` is SUCCESS (no compile errors).
- [ ] The firmware .bin exists at a reported absolute path.
- [ ] `platformio.ini`, `src/main.cpp`, any demo/web source are all in `/home/brad/projects/oneboard/`.
- [ ] The 3 demos are defined and switchable at runtime via the web set-endpoint.
- [ ] Last-selected demo persists across reboot (NVS).
- [ ] Report is honest: what compiled, what's unverified (anything needing hardware eyes).

## Commit

If a git repo is initialized in `/home/brad/projects/oneboard` (init one if not, it's the project root), commit after each Task completes with a clear message like `feat: stage1 task N — <what>`. The worker may commit (the project belongs to this effort), but must NEVER push anywhere or touch other projects.

## Brad's test loop (after build)

1. `bin/PROGRAM.sh` isn't needed — flash with:
   `pio run -t upload` (if USB detected) OR esptool with the .bin + Brad reports the port.
2. Connect to WiFi SSID `OneBoard-Stage1`.
3. Open http://192.168.4.1/ — tap Timer / Temp Logger / Reaction Game.
4. Verify Reaction Game responds to touch (proves XPT2046 wiring).
5. Power-cycle — verify the last demo comes back (proves NVS persistence).
6. Report back to Bob: what works, what's broken, what the screen shows.