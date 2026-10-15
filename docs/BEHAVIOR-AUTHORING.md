# OneBoard behavior authoring — reference for LLMs and humans

Write behavior definitions as JSON. The board compiles them to bytecode at load
and becomes the tool — no reflash. Validate before loading:
`python3 tools/validate_behavior.py your_behavior.json`

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
  and is named `"touch-down"` (full-screen touch dispatch).

## Guards (structured, never string-eval)

`eq ne lt le gt ge` (compare `a` vs `b`), `is_true` (a), `elapsed_ge`
(stamp `a` was ≥ `b` ms ago), `always`.

## Verbs (straight-line lists — no loops, no recursion, no expressions)

| Verb | Payload | Notes |
|------|---------|-------|
| `add`/`sub` | `{"slot": n, "delta": {"lit": 1000}}` | `delta` may be `{"noise":{"lo":-2,"hi":2}}` (re-sampled every tick) |
| `set`/`setf` | `{"slot": n, "value": {...}}` | `setf` for floats |
| `toggle` | `{"slot": n}` | bool flip |
| `stamp` | `{"slot": n}` | n = now (ms) |
| `now_minus` | `{"slot": n, "from": {...}}` | n = now - from |
| `format` | `{"slot": str, "op": "ms"\|"temp"\|"react"\|"int", "src": {...}}` | builds display string |
| `progress` | `{"slot": w, "src": {...}, "max": {...}, "px": {...}}` | w = progress in px |
| `render` | see below | draw a primitive |
| `if` | `{"guard": {...}, "then": [verbs], "else": [verbs]?}` | bounded forward branch only |

## Render primitives

Every render is one object; **fields must match the type** (validator enforces).
Colors: `black white red green blue yellow cyan magenta orange navy darkgrey
darkgray grey gray`.

- `clear` → `{"type":"clear","color":"black"}` (repaint whole screen; first verb
  of each screen's render set)
- `text` → `{"type":"text","x","y","src","font","color","center"?}`
  - `src` = `{"lit":"...text..."}` or `{"slot":"string_slot"}`
  - `font` 1-7 (`{"lit":2}` normal, `{"lit":6}` big)
  - `center: true` = x is the horizontal centre (text is centred on screen)
- `rect` / `fillrect` → `{"x","y","w","h","color"}`
- `line` → `{"x","y","w","h","color"}` (x,y = p1; w,h = p2)
- `circle` → `{"x","y","r","color"}`
- `bar` → `{"x","y","w","h","color","src","max","px"}` (w may be `{"slot":...}`)
- `graph` → `{"x","y","w","h","color","slot"}` (`slot` = float slot to graph)

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
2. Each screen's render set starts with `clear`
3. Slot names in guards/verbs/render match `state` exactly (validator checks)
4. Every text render has a `src` (`{"lit":...}` or a string slot)
5. Reaction/state-machine phases: model phase as an int64 slot + guarded handlers
