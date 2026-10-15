# Stage 2 — Behavior-Store over WiFi (tools as JSON data)

> **For the worker:** Build this on top of the Stage 1 project at `/home/brad/projects/oneboard/`.
> Tool definitions become JSON *data* the board loads over WiFi, instead of hardcoded C++ demos.
> You are a headless kanban worker on DeepSeek-V4-Flash. Call kanban_complete with your CONTRACT's EXIT STATE when done.

**Goal:** The 3 Stage 1 demos (timer, temp-logger, reaction-game) are no longer hardcoded — they
live as JSON "behavior definitions" on the machine, and the board loads them over WiFi from a
behavior store. Swapping a tool = changing JSON, not reflashing.

**Constraint (carry-over):** the JSON schema this stage introduces MUST be something a tiny VM can
execute in Stage 3 (~20-30 verb vocabulary, 300KB SRAM, no arbitrary code). Design it data-shaped,
not code-shaped.

---

## Design: the behavior-definition schema (locked for Stage 2 & 3)

> ⚠️ **Contract (adopted)**: This schema is governed by the **8-point behavior contract** at the
> bottom of `docs/GPT-HANDOFF.md` (GPT's refinement, current of record). Where anything in this
> plan conflicts with that contract, **the contract wins**. In particular: state refs are
> `{"slot":"name"}` (never string-eval like `format_ms(state.elapsed_ms)` — replace that with an
> explicit `format_ms` verb taking slot refs), execution is first-match-guard-wins, and touch
> reactivity uses named touch-region events rather than an ad-hoc `on_touch` string.

Each "tool" is one JSON object. Small fixed vocabulary, typed values, no functions/expressions.

```
{
  "id": "timer",                    // unique tool id (also used in URLs)
  "name": "Count-Up Timer",
  "layout": "fullscreen",            // base screen template
  "state": [                         // declarative state slots (typed)
    {"name": "elapsed_ms", "type": "int64", "init": 0},
    {"name": "running",    "type": "bool",  "init": false}
  ],
  "on_tick": "tick",                 // named behavior, 1Hz by default
  "behaviors": {                     // named event handlers (tiny verb programs)
    "tick": [
      {"v": "if", "args": ["state.running", "is_true"], "then": [
        {"v": "add", "args": ["state.elapsed_ms", {"v": "lit", "args": "1000"}]},
        {"v": "render", "args": ["text", {"x": 20, "y": 90, "src": "format_ms(state.elapsed_ms)", "font": 4, "color": "white"}]}
      ]}
    ],
    "on_touch": [
      {"v": "toggle", "args": ["state.running"]}
    ]
  },
  "on_first_render": "render"
}
```

**Verb set (Stage 2 = subset, Stage 3 = full VM):**
- `lit`    — literal value (number/string/bool)
- `if`     — conditional branch (`args[0]` condition, `then`/`else` lists)
- `is_true` — bool predicate
- `add` / `sub` — arithmetic (assign to state)
- `toggle` — flip a bool
- `render` — draw primitive: `text` (x,y,src,font,color) | `rect` | `line` | `circle` | `bar`
- `format_ms` — int → "MM:SS" helper
- (Stage 3 full set adds: `tap_wait_random`, `stamp_ms`, `read_temp`, more predicates.)

**Rules:** no loops, no arbitrary expressions, no function definitions — only named behavior
blocks made of bounded verbs. This stays VM-friendly.

---

## Task 1: Expose the store over WiFi (server side, on the gateway machine)

**Objective:** A tiny store on the machine that serves the JSON definitions; the board fetches them.

**Files:**
- Create: `/home/brad/projects/oneboard/behaviors/timer.json`
- Create: `/home/brad/projects/oneboard/behaviors/temp-logger.json`
- Create: `/home/brad/projects/oneboard/behaviors/reaction-game.json` (using the schema above —
  temp-logger draws a rolling line graph via `line` verbs; reaction-game uses a
  `tap_wait_random`-style step)
- Create: `/home/brad/projects/oneboard/store/` — a dead-simple static JSON host (a Python
  `http.server` on a fixed port, or one you can run with a one-liner; keep it minimal). Serves
  the `behaviors/` dir as JSON at `http://<machine-ip>:PORT/<id>.json`.

**Step 1:** Write the 3 JSON definitions per the schema. Keep them small and correct.
**Step 2:** Run the store and confirm each URL returns valid JSON:
  Run: `python3 -m http.server 8080 --directory /home/brad/projects/oneboard/behaviors`
  Then `curl http://127.0.0.1:8080/timer.json` → valid JSON, HTTP 200.
**Step 3:** Commit.

---

## Task 2: Board loads a behavior over WiFi

**Objective:** The ESP32 fetches a JSON behavior from the store over its WiFi **station** connection,
parses it, and runs it. Keep Stage 1's soft-AP fallback so a store URL can be set.

**Files:**
- Modify: `platformio.ini` (add `arduino-json` lib if not present; keep TFT_eSPI)
- Modify: `src/main.cpp`

**Step 1:** Add ArduinoJson dependency:
  `lib_deps = bodmer/TFT_eSPI@^2.5.0, bblanchon/ArduinoJson@^7.0.0`
**Step 2:** WiFi: connect as STATION to a configurable SSID/pass (compile-time or a stored NVS
string), then HTTP GET `<store-url>/<id>.json` (the class `HTTPClient` with a ~5s timeout).
Parse with ArduinoJson into the schema structures.
**Step 3:** Keep a fallback: if the store isn't reachable, fall back to Stage 1's demo mode so the
board is never bricked/totally blank.
**Step 4:** Commit.

---

## Task 3: Tiny interpreter loop — load a behavior, dispatch its verbs

**Objective:** `loop()` runs the active behavior's `behaviors` blocks by dispatching verbs from the
JSON (this is the seed of the Stage 3 VM — keep it minimal and correct).

**Files:**
- Modify: `src/main.cpp`

**Step 1:** Parse the loaded JSON into a small C++ struct/class representation: `Behavior { id; name;
state slots; behaviors map }`. Render primitives (`text`, `rect`, `line`, `circle`, `bar`) become
thin wrappers around TFT_eSPI calls.
**Step 2:** Implement a `runBlock(name)` that walks the verb list and executes each verb via a switch
on verb string. Implement the Stage-2 subset: `lit`, `if`, `is_true`, `add`, `sub`, `toggle`,
`render`, `format_ms`.
**Step 3:** Hook the demo triggers: timer runs `tick` every 1s; reaction-game runs on tap via touch
+ random-wait steps; temp-logger reads a simulated sensor and renders the graph.
**Step 4:** This replaces the hardcoded `setActiveDemo` switching with "load JSON → run its
behaviors." When the web page or a `/load?id=<id>` route requests a tool, the board fetches the JSON
and swaps to it.
**Step 5:** Commit.

---

## Task 4: Wire store selection into the web page + verify

**Objective:** The Stage 1 control page becomes a tool launcher that loads behavior by id from the store.

**Files:**
- Modify: `src/main.cpp` (web routes)

**Step 1:** Update the web page: 3 buttons ("Timer", "Temp Logger", "Reaction Game") now hit
`/load?id=<id>` instead of just `setActiveDemo`. The board fetches `<store-url>/<id>.json`, parses,
swaps behavior.
**Step 2:** Keep `/status` returning the active behavior id + which verbs it used (simple tally) so
Brad/probes can confirm the interpreter is running the JSON.
**Step 3:** Compile. Expected: `SUCCESS`.
**Step 4:** Report the store URL scheme + the exact SSID/wifi config Brad must set, and how to test.

---

## Acceptance criteria

- [ ] `behaviors/*.json` exist, valid JSON, schema-conformant (3 tools).
- [ ] Store serves them over HTTP (curl-verified).
- [ ] `pio run` is SUCCESS.
- [ ] Board loads a JSON behavior over WiFi, parses it (ArduinoJson), runs the verb subset.
- [ ] Web `/load?id=<id>` swaps tools; `/status` reports active tool.
- [ ] Soft-AP/Stage-1 fallback still works if store unreachable (no brick).
- [ ] Everything lives in `/home/brad/projects/oneboard/`.
- [ ] Report is honest about what's unverified (anything needing hardware eyes).

## Commit

The repo is git-initialized at `/home/brad/projects/oneboard/`. Commit after each task with a clear
message (`feat: stage2 task N — ...`). Never push anywhere. Never touch other projects.