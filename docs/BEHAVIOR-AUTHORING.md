# OneBoard behavior authoring — reference for LLMs and humans

Write behavior definitions as JSON. The board compiles them to bytecode at load
and becomes the tool — no reflash. Validate before loading:
`python3 tools/validate_behavior.py your_behavior.json`

> **Load failures are loud, not silent.** The compiler rejects unknown verbs,
> unknown slots, unknown format/render/guard ops, and limit overflows — a bad
> behavior does NOT load "ok". `/load` returns `{"ok":false,"error":"<reason>"}`
> (e.g. `unknown verb 'increment'`, `set: unknown slot 'missing'`), and the web
> UI alerts it. These checks reject the errors listed above; they do not prove a tool
> implements the behavior its author intended.

## Structure

```json
{
  "id": "my-tool",
  "name": "Human-Readable Name",
  "layout": "fullscreen",
  "state":  [ ...typed slots... ],
  "events": [ ...named events, ordered handlers... ]
}
```

## State slots (typed, fixed — no dynamic variables)

```json
{"name": "elapsed_ms", "type": "int64",  "init": 0},
{"name": "temp",       "type": "float",  "init": 72.0},
{"name": "running",    "type": "bool",   "init": true},
{"name": "label",      "type": "string", "init": "00:00"}
```

Types: `int64`, `float`, `bool`, `string`. The VM never creates variables —
every value references one of these slots.

## Events & handlers

```json
{
  "name": "tick",
  "schedule": {"type": "interval", "ms": 1000},
  "handlers": [
    {"guard": {"op": "eq", "a": {"slot": "running"}, "b": {"lit": true}},
     "run": [ ...verbs... ]}
  ]
}
```

- **First-match-guard-wins**: on an event, run the FIRST handler whose guard
  matches, then stop. Order matters.
- Guard-less handler = always matches.
- `schedule` interval for ticking events. A touch event has **no** `schedule`
  and is named `"touch-down"`.
- **Touch regions (buttons).** A touch event can declare an optional `region`
  rectangle; the event only fires when the press lands inside it:
  ```json
  {"name": "touch-down", "region": {"x": 0, "y": 200, "w": 240, "h": 120},
   "handlers": [...]}
  ```
  No `region` = full screen (every press fires). Use regions to build buttons:
  multiple touch events, each with its own rectangle, on the same screen.

## Rendering is frame-flush, not instruction-snapshot

`render` verbs do NOT draw immediately. They mark a primitive dirty; the board
draws all dirty primitives **after the whole handler runs**, reading the state
as it is then. So inside one handler, **mutate state first, then render** — a
handler that does `set label=A → render(label) → set label=B` renders B, not A.
The validator warns when a render reads a slot a later verb in the same handler
writes (this exact footgun). If you need two different frames, use two handlers
(e.g. a tick phase machine), not two render calls around a mid-handler write.

## Guards (structured, never string-eval)

`eq ne lt le gt ge` (compare `a` vs `b`), `is_true` (a), `elapsed_ge`
(stamp `a` was ≥ `b` ms ago), `always`.

## Verbs (straight-line lists — no loops, no recursion, no expressions)

| Verb | Payload | Notes |
|------|---------|-------|
| `add`/`sub` | `{"slot": n, "delta": {"lit": 1000}}` | `delta` may be `{"noise":{"lo":-2,"hi":2}}` (re-sampled every tick) |
| `set`/`setf` | `{"slot": n, "value": {...}}` | `setf` for floats |
| `toggle` | `{"slot": n}` | bool flip |
| `stamp` | `{slot}` | slot = now (ms) |
| `random` | `{slot, min, max}` | slot = random int in [min,max] inclusive (e.g. dice 1-6) |
| `now_minus` | `{slot, from}` | slot = now - from |
| `format` | `{"slot": str, "op": "ms"\|"temp"\|"react"\|"int", "src": {...}}` | builds display string |
| `progress` | `{"slot": w, "src": {...}, "max": {...}, "px": {...}}` | w = progress in px |
| `render` | see below | draw a primitive |
| `if` | `{"guard": {...}, "then": [verbs], "else": [verbs]?}` | bounded forward branch only |

## Render primitives

Every render is one object; **fields must match the type** (validator enforces).
Colors: `black white red green blue yellow cyan magenta orange navy darkgrey
darkgray grey gray`.

- `clear` → `{"type":"clear","color":"black"}` (repaint the whole screen: the
  first verb of each screen's render set, and the right tool when a screen
  genuinely changes). It is a **full-screen fill — ~61 ms of SPI at 20 MHz**, so
  it must never sit in a repeating interval handler: at a 100 ms tick the panel
  is black 61% of the time and visibly flickers. The validator warns on this.
- `text` → `{"type":"text","x","y","src","font","color","center"?,"bg"?,"w"?,"h"?}`
  - `src` = `{"lit":"...text..."}` or `{"slot":"string_slot"}`
  - `font` 1-7 (`{"lit":2}` normal, `{"lit":6}` big)
  - `center: true` = x is the horizontal centre (text is centred on screen)
  - `bg` = an erase colour: the text blanks its own box before drawing, so a
    value that changes every tick needs no `clear` (~6 ms vs ~61 ms). Leave it
    out for text drawn over a button or a graph — it would paint over them.
  - `w` / `h` = reserve a fixed erase box, for a value whose width changes (a
    clock, a millisecond counter). Size `w` for the *longest* string, or the tail
    of the previous longer string is left behind.
- `rect` / `fillrect` → `{"x","y","w","h","color"}`
- `line` → `{"x","y","w","h","color"}` (x,y = p1; w,h = p2)
- `circle` → `{"x","y","r","color"}`
- `bar` → `{"x","y","w","h","color","src","max","px"}` (w may be `{"slot":...}`)
- `graph` → `{"x","y","w","h","color","slot"}` (`slot` = float slot to graph).
  The graph erases its own plot interior (to black — keep graphs on a black
  background) before drawing the trace, so it needs no `clear` and can update on
  a fast tick.

## Value refs — the #1 gotcha

Coordinates, sizes, values, text, fonts are ALWAYS `{"lit": N}` / `{"lit": "s"}`
or `{"slot": "name"}`. A bare `80` or `"text"` works in the lenient compiler but
**wrapped objects are the contract** — and a value wrapped as `{"lit":N}` whose
field is misspelled compiles to 0/empty silently. Misspelled field names are the
most common failure on hardware. The validator catches these; the board won't.

## Limits

12 slots, 20 render primitives, 80 instructions, 6 events, bounded forward `if`
only. Display is 240x320 portrait.

## Checklist before flash

1. `python3 tools/validate_behavior.py my-tool.json` → VALID
2. Each NEW screen's first frame starts with `clear` — and **no** `clear` in any
   repeating handler (use text `bg`, or `fillrect` the region you redraw)
3. Slot names in guards/verbs/render match `state` exactly (validator checks)
4. Every text render has a `src` (`{"lit":...}` or a string slot)
5. Reaction/state-machine phases: model phase as an int64 slot + guarded handlers

## VM regression checks

The native suite compiles the actual `src/vm.cpp` with ArduinoJson 7 and
lightweight Arduino/display stubs; it does not simulate electrical hardware.
It checks compiler limits and slot references, progress defaults, initial render
branch selection without state mutation, and compilation of all shipped tools.
GitHub Actions runs it with AddressSanitizer alongside the firmware build matrix.

To run locally with g++ and an ArduinoJson 7 checkout:

```bash
ARDUINOJSON_DIR=/path/to/ArduinoJson tests/native/run.sh
```

Initial-screen selection evaluates nested `if` guards against initial state and
marks only the selected branches for drawing. It does not execute state writes.
Slot names must be unique, nonempty, and at most 15 characters. Invalid slot
declarations and references, and slot/event/handler/guard/instruction/render
capacity overflows, fail compilation rather than truncating the tool.
