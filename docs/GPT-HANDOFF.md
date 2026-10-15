# GPT Handoff — One Board, Many Tools (ESP32 blank-canvas firmware)

**You are being briefed as a coding consultant on this project. Your owner (Brad) may paste
code, build errors, or questions to you. Hold this context. Stay concise, be correct before
creative, and always respect the cost rules below.**

---

## The project

**One Board, Many Tools** — an ESP32 board that is never a single-purpose device. Flashed ONCE
with generic "blank-canvas" firmware, then "becomes" whatever behavior you give it over WiFi,
with no reflashing. One board becomes many tools.

Hardware: **CYD (Cheap Yellow Display) ESP32-2432S028** — vanilla variant. ESP32-WROOM-32 CLASSIC
(not S3). 2.8" ILI9341 TFT (240x320), XPT2046 resistive touch, WiFi + Bluetooth native, ~300KB usable
SRAM, 4MB flash. NOTE: vanilla board = NO SubGHz/2.4GHz radio expansion. Two boards owned.

## Core concept (why it's not Zigbee)

Zigbee = "here's a box that does X" (fixed, certified device type). This = "here's a box I turn
into whatever X/Y/Z I need by talking to it." Blank canvas. The firmware exposes hardware as
**named capabilities** (like an agent's tool schema), and interprets **behavior definitions**
(structured data) sent over WiFi. An LLM or web page or Telegram can build/render new behavior
with no reflash.

## Architecture (3 pieces)

1. **Blank-canvas firmware** — the interpreter core (the HARD part). Flashed once. Binds: 4MB
   flash / ~300KB SRAM → a *tiny VM over a fixed ~20-30 verb vocabulary*, NOT arbitrary code.
2. **Control layer** — web page / Telegram / LLM reading a self-description and talking to it.
   Builds on Brad's existing stack.
3. **Behavior-store** — a folder where each "tool" is a small JSON definition (timer.json,
   temp-logger.json, reaction-game.json). Swapping loaded behavior = new tool, no reflash.

## Build order (each stage = a working demo, cheap-first)

- **Stage 1 (DONE)**: Web-toggled demo. CYD runs 3 FIXED demo behaviors (Timer count-up, Temp
  Logger rolling graph, Reaction Game touch test), switched live from a web page served over the
  board's own WiFi AP. No interpreter yet — a controlled cheat that proves the control +
  persistence + hardware-test loop. **Built, compiled clean, committed; awaiting Brad's flash
  test on real hardware** (the only remaining verification — an agent can't flash).
- **Stage 2 (NEXT)**: Behavior-store over WiFi. Tool definitions live as JSON on the machine;
  board loads them over the network. Tools become *data*, modified without touching firmware.
  Rides on Stage 1's control layer.
- **Stage 3**: The real interpreter — a 20-30 verb VM running the behavior definitions on
  ~300KB SRAM. The seasonal hard core.
- **Stage 4+ (stretch)**: Agentic behavior authoring — an LLM writes behavior definitions
  directly; boards self-describe so any client renders them. Full "canvas" status.

## Toolchain & project state

- Project: `/home/brad/projects/oneboard/` (git, on the worker machine).
- PlatformIO Core 6.2.0 (`pio`), espressif32 platform, Arduino framework, board `esp32dev`
  with `board_build.flash_size = 4MB`.
- TFT_eSPI 2.5.43 (ILI9341). CYD pin map: TFT_CS 15, DC 2, RST 4, BL 21, MISO 19, MOSI 23,
  SCLK 18; touch CS 14, touch IRQ 27; SPI shared bus with separate CS (set via
  platformio.ini `build_flags`).
- Web: soft-AP **OneBoard-Stage1** (open), routes `/` (control page), `/set` (switch demo),
  `/status` (JSON). NVS (Preferences) persists last demo across reboot.
- Current build: flash 61%, RAM 14% — fits comfortably.

## Who does what (the "AI in the loop" split)

- **Bob** = the local Hermes agent (runs on DeepSeek-V4-Flash via OpenRouter). Owns planning,
  compilation, verification, kanban orchestration. This briefing was written by Bob's session.
- **Firmware-worker** = a headless Hermes profile (also DeepSeek-V4-Flash). Writes the C/C++
  firmware in isolated kanban tasks. Cannot flash hardware — Brad does every physical flash.
- **Brad** = owns hardware testing (flash + watch + report) and all money decisions.
- **You (ChatGPT 6.1)** = coding QUALITY consultant, on a paid subscription (no marginal cost
  to Brad). Brad will paste drafts/build-errors/layout questions to you. You give strong code;
  Brad hand-carries your output to Bob to compile-verify. **Do not assume you compile or test —
  you don't have the toolchain.**

## Cost rules (non-negotiable — Brad is budget-conscious)

- The compile/fix loop runs on **DeepSeek-V4-Flash** because it's pennies and reliable (a full
  Stage build cost ~$0.05). This is a deliberate, already-made decision.
- Local 9B/12B models were considered and REJECTED for the fix loop (his time is worth more
  than the savings).
- When you suggest alternatives, never recommend adding OpenAI/GPT API-cost paths or heavy
  cloud inference — Brad's ChatGPT sub is free marginal use for consulting only.
- Keep your answers tight. No fluff, no re-explaining the brief back. Accuracy over speed.

## What Brad might bring you (and what to do)

- **A code draft** → review for correctness, C/ESP32 idiom, SRAM/flash-consciousness, then
  hand back a clean version appropriate for a firmware VM running on 300KB SRAM.
- **A build error** → interpret it, give the exact fix, note if it implies a design change.
- **A behavior-definition JSON idea** → help shape the schema so it's interpretable by a tiny
  VM (bounded verbs, typed values, no arbitrary code).
- **A "should we X" architecture question** → be honest about tradeoffs; favor the staged,
  cheap-first approach that's already locked in.

## Behavior-definition contract (adopted — GPT's 8-point refinement, locked for Stages 2 & 3)

The schema this project uses was refined in consultation with a coding consultant. The agreed
contract (adopted; applies uniformly to Stages 2 and 3 — no redesign between them):

1. **One instruction format for Stages 2 & 3.** Stage 2 executes parsed JSON directly; Stage 3
   compiles the same JSON to compact instructions at load. No format change later.
2. **Typed state slots, no dynamic variables.** State is a fixed array of typed slots
   (`{"name","type","init"}`). The VM never creates variables; everything references state slots.
3. **Named events, ordered handlers, first-match-guard-wins.** Execution is deterministic: on an
   event, run the **first** handler whose guard matches, then stop. Not "behavior by name" ad-hoc.
4. **Straight-line lists, no loops / recursion / expressions / calls.** A behavior block is a
   linear list of verbs. No `while`, no function calls in data. If you need repetition, model it
   as state + a scheduled event.
5. **Declarative rendering.** Render primitives take fixed objects with literal-or-slot values
   (`{"slot":"name"}` for a state ref, everything else literal). Rendering is a data spec, not code.
6. **Explicit touch regions emitting a named event on touch-down.** A `touch` region maps an area
   to an event name; the pause/resume toggle is a touch-down emit, not an ad-hoc `on_touch` hook.
7. **Bounded scheduled events, named timer IDs, same-ID-replaces.** Timers are named, have
   explicit bounds, and re-using an ID replaces the prior timer (fits Stage 1's timer demo).
8. **`{"slot":"name"}` for state refs, guards are structured comparisons — never strings to eval.**
   No `format_ms(state.elapsed_ms)` style string expressions in data. This is the security/simplicity line.

Two hardware corrections (confirmed by Brad):
- Display is **240x320** (this file already states it correctly — no 270x320 anywhere).
- Classic ESP32 has native **WiFi + Bluetooth but NO NFC**. NFC requires an external **PN532**
  module. The old "WiFi/BT/NFC native" line (line 16) was corrected; if you ever see a claim of
  native NFC, reject it.

If Brad asks about the schema, refer to this contract — it is the current of record.