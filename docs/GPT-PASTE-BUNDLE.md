# PASTE THIS TO GPT — One Board, Many Tools (current as of Oct 15 2026)

> Copy everything from the horizontal rule below through the end. It's a self-contained briefing:
> the project brief, current stage status, the behavior-definition contract, a worked example, and
> the open Stage 3 question.

---

## Project & current status

**One Board, Many Tools** — an ESP32 board flashed ONCE with generic "blank-canvas" firmware that
"becomes" whatever behavior you give it over WiFi, with no reflashing. One board becomes many tools.

Hardware: **CYD (Cheap Yellow Display) ESP32-2432S028** — vanilla. ESP32-WROOM-32 CLASSIC (not S3).
2.8" ILI9341 TFT (**240x320**), XPT2046 resistive touch, **WiFi + Bluetooth native but NO native
NFC** (needs external PN532). ~300KB usable SRAM, 4MB flash. Two boards owned.

Stage status:
- **Stage 1 DONE** — web-toggled demo, 3 fixed behaviors, soft-AP, NVS. Compiles clean.
- **Stage 2 DONE (contract-conformant)** — behavior-store over WiFi: JSON tool definitions served
  to the board, fetched, cached in LittleFS, soft-AP fallback. ~15-verb interpreter dispatches them.
  Reconciled (`7ed8043`) so **schema == execution model**. `pio run` SUCCESS (79.4% flash, 14.8% RAM).
- **Stage 3 NEXT** — the real interpreter: compile the SAME JSON to compact instructions on ~300KB
  SRAM. Because Stage 2's schema already matches the contract, no format change / retrofit.
- **Stage 4 stretch** — agentic behavior authoring (LLM writes JSON, boards self-describe).

Toolchain: PlatformIO Core 6.2.0, espressif32, Arduino framework, TFT_eSPI 2.5.43, ArduinoJson v7.
Web: soft-AP "OneBoard-Stage2", routes `/` `/load` `/status` `/store`. Agent (Bob) + headless worker
run DeepSeek-V4-Flash; cost is pennies for a full Stage build. Brad owns every physical flash test.

Who you are: coding QUALITY consultant (paid sub, no marginal cost). Review code/errors/JSON for C
idiom and SRAM/flash-consciousness. You don't compile or test. Never suggest GPT/OpenAI API-cost
build paths. Tight answers, correct before creative.

## Behavior-definition contract (locked for Stages 2 & 3 — schema of record)

1. **One instruction format for Stages 2 & 3.** Stage 2 executes parsed JSON directly; Stage 3
   compiles the same JSON to compact instructions at load. No format change later.
2. **Typed state slots, no dynamic variables.** State is a fixed array of typed slots
   (`{"name","type","init"}`). The VM never creates variables; everything references state slots.
3. **Named events, ordered handlers, first-match-guard-wins.** On an event, run the **first**
   handler whose guard matches, then stop. Not "behavior by name" ad-hoc.
4. **Straight-line lists, no loops / recursion / expressions / calls.** A behavior block is a linear
   list of verbs. No `while`, no function calls in data. Repetition = state + a scheduled event.
5. **Declarative rendering.** Render primitives take fixed objects with literal-or-slot values
   (`{"slot":"name"}` for a state ref, everything else literal). Rendering is a data spec, not code.
6. **Explicit touch regions emitting a named event on touch-down.** A region maps an area to an
   event name; pause/resume is a touch-down emit, not an ad-hoc `on_touch` hook.
7. **Bounded scheduled events, named timer IDs, same-ID-replaces.** Timers are named, bounded, and
   re-using an ID replaces the prior timer.
8. **`{"slot":"name"}` for state refs, guards are structured comparisons — never strings to eval.**
   No `format_ms(state.elapsed_ms)` style string expressions in data. Security/simplicity line.

## Worked example — the contract schema in practice (timer.json)

```json
{
  "id": "timer",
  "name": "Count-Up Timer",
  "layout": "fullscreen",
  "state": [
    {"name": "elapsed_ms", "type": "int64",  "init": 0},
    {"name": "running",    "type": "bool",   "init": true},
    {"name": "elapsed_txt","type": "string", "init": "00:00"},
    {"name": "progress_w", "type": "int64",  "init": 0}
  ],
  "events": [
    {
      "name": "tick",
      "schedule": {"type": "interval", "ms": 1000},
      "handlers": [
        {
          "guard": {"op": "eq", "a": {"slot": "running"}, "b": {"lit": true}},
          "run": [
            {"add": {"slot": "elapsed_ms", "delta": {"lit": 1000}}},
            {"format": {"slot": "elapsed_txt", "op": "ms", "src": {"slot": "elapsed_ms"}}},
            {"progress": {"slot": "progress_w", "src": {"slot": "elapsed_ms"}, "max": {"lit": 3600000}, "px": {"lit": 220}}},
            {"render": {"type": "clear", "color": "black"}},
            {"render": {"type": "text", "x": {"lit": 20}, "y": {"lit": 60}, "src": {"lit": "Timer"}, "font": {"lit": 2}, "color": "yellow"}},
            {"render": {"type": "text", "x": {"lit": 50}, "y": {"lit": 110}, "src": {"slot": "elapsed_txt"}, "font": {"lit": 6}, "color": "green"}},
            {"render": {"type": "bar", "x": {"lit": 10}, "y": {"lit": 170}, "w": {"slot": "progress_w"}, "h": {"lit": 10}, "color": "blue"}},
            {"render": {"type": "text", "x": {"lit": 30}, "y": {"lit": 210}, "src": {"lit": "Touch to pause/resume"}, "font": {"lit": 2}, "color": "white"}}
          ]
        }
      ]
    },
    {
      "name": "touch-down",
      "region": {"x": 0, "y": 0, "w": 240, "h": 320},
      "handlers": [
        {"run": [{"toggle": {"slot": "running"}}]}
      ]
    }
  ]
}
```

Verb vocabulary currently implemented: `add`, `sub`, `set`, `setf`, `toggle`, `stamp`, `now_minus`,
`format` (op ms/temp), `progress`, `render` (clear/text/rect/fillrect/line/circle/bar/graph), `if`.
Guard ops: `eq`, `ne`, `lt`, `le`, `gt`, `ge`, `is_true`, `elapsed_ge`, `always`. Values:
`{"slot":..}` | `{"lit":..}` | `{"noise":{lo,hi}}` | literal.

## The open question I need your design input on (Stage 3)

Given this contract and the running Stage 2 interpreter, **how should the Stage 3 VM compile this
same JSON into compact instructions on ~300KB SRAM without retrofitting the schema?** I want a
concrete design: how verbs/guards/values map to a compact bytecode (or packed representation), a
fixed ~20-30 verb opcode table, how render params get flattened, and rough SRAM/flash sizing.
Favor the deterministic, compile-once-at-load model per contract point 1. This is the de-risking
design decision before I dispatch the Stage 3 worker.

---