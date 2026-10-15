# OneBoard

A **self-installing, multi-client hardware canvas** built on the ESP32 CYD
(Cheap Yellow Display, 240×320). One board, many tools: it becomes whatever
you describe — a timer, a temp logger, a reaction game, a tap counter, a
stopwatch, a dice roller — **without ever reflashing the firmware**.

Any client can drive it and render its screen from the board's own
self-description: a web page, a Telegram bot, or an LLM.

---

## What makes it different

- **No serial cable, no BOOT button.** First boot opens a captive portal —
  join the board's WiFi from your phone and the setup page auto-opens.
- **Behavior = data, not firmware.** A tool is a JSON description compiled to
  bytecode on the board at load time. New tools ship over WiFi from a store,
  not over USB.
- **Author with an LLM.** `tools/author.py "I want a dice roller"` → the model
  writes a schema-valid behavior, validates it, self-corrects, and pushes it
  to the live board. Done.
- **Any client renders it.** `/schema` describes the VM vocabulary, `/state`
  reports live values, and `tools/board_view.html` draws the board's screen in
  a browser from that description alone.
- **Controllable from Telegram.** A thin no-LLM bot (near-zero cost) drives
  `/load`, `/status`, `/tools`, `/schema` from your phone, anywhere.

---

## Hardware

| | |
|---|---|
| Board | CYD ESP32-2432S028R (HaleHound / "cheap yellow display") |
| Display | ILI9341 240×320 portrait, `setRotation(0)`, no inversion |
| Touch | XPT2046 on separate VSPI (CS33/MOSI32/MISO39/CLK25/IRQ36), 250 ms debounce, one event per press, no pull-up on IRQ36 |
| Display SPI | HSPI (MISO12/MOSI13/SCLK14/CS15/DC2/RST=-1/BL21) — `USE_HSPI_PORT` mandatory |
| MCU | ESP32 (espressif32 platform, Arduino framework), 4 MB flash |

`platformio.ini` carries the full pin map and all TFT/touch build flags.

### The CYD pin problem (why the config matters)
The CYD is notorious for white-screen / broken-touch if the TFT and touch
share a SPI bus or the wrong port is used. This repo's `platformio.ini` is the
verified-good config: display on HSPI, touch on its own VSPI, `USE_HSPI_PORT`
set, fonts 2/4/6 loaded, `SPI_FREQUENCY 20 MHz`, touch 2.5 MHz. Do not "fix"
what is not broken here.

---

## Architecture

```
        +---------------------+        HTTP (store)         +------------------+
        |   tools/author.py   | --------------------------> |  store/serve.py  |
        |   (LLM authoring)    |   writes behaviors/*.json  |  (behavior store)|
        +---------------------+                             +--------+---------+
                                                                     |
        +---------------------+   /load  /store  /wifi   <-----------+
        |   web page (board)  | <====================================+
        +---------------------+     (compiled to bytecode, no reflash)
        +---------------------+   /status /state /schema
        |  board_view.html    | <------------------------------------+
        |  (browser renderer) |                                       |
        +---------------------+                                       |
        +---------------------+   /status /tools /load /schema        |
        |  oneboard_bot.py    | <------------------------------------+
        |  (Telegram control) |     (thin, no-LLM)
        +---------------------+
```

**Firmware side (`src/`):**
- `main.cpp` — HTTP server, WiFi provisioning, captive portal, behavior loader
- `vm.h` / `vm.cpp` — the bytecode VM: 26 instruction types, 9 guard ops,
  JSON→bytecode compilation at load (ArduinoJson on the cold path only)

**Host side (`tools/`, `store/`, `systemd/`):**
- `author.py` — LLM behavior authoring pipeline (OpenRouter/DeepSeek-V4-Flash)
- `validate_behavior.py` — schema validator (catches format errors, not
  semantic lies — hardware is the only true gate)
- `board_view.html` — any-client screen renderer (browser)
- `oneboard_bot.py` + `oneboard-tg-bot.service` — Telegram control
- `serve.py` — the behavior store (this machine, port 8082)

---

## Behavior model (the "language" of a tool)

A behavior is JSON with **typed state slots** and **event handlers**:

```json
{
  "id": "my-tool",
  "name": "Human-Readable Name",
  "state": [
    {"name": "running", "type": "bool",   "init": true},
    {"name": "elapsed", "type": "int64",  "init": 0},
    {"name": "label",   "type": "string", "init": "00:00"}
  ],
  "events": [
    {"name": "tick", "schedule": {"type": "interval", "ms": 1000},
     "handlers": [{"guard": {"op": "eq", "a": {"slot": "running"}, "b": {"lit": true}},
                   "run": [ ...verbs... ]}]}
  ]
}
```

- **Slots** are typed (`int64`, `float`, `bool`, `string`) and fixed — the VM
  never creates variables, every value references a slot. Max 12.
- **First-match-guard-wins**: on an event, run the first handler whose guard
  matches, then stop. Order matters.
- **Verbs** are straight-line lists (no loops, no recursion): `add`, `sub`,
  `set`/`setf`, `toggle`, `stamp`, `random`, `now_minus`, `format`,
  `progress`, `render`, `if` (bounded forward branch).
- **Render primitives**: `clear`, `text`, `rect`, `fillrect`, `line`,
  `circle`, `bar`, `graph`. Colors from a fixed palette.
- **Guards**: `eq ne lt le gt ge`, `is_true`, `elapsed_ge`, `always`.
- **Limits**: 12 slots, 20 render primitives, 80 instructions, 6 events.

Full authoring reference: [`docs/BEHAVIOR-AUTHORING.md`](docs/BEHAVIOR-AUTHORING.md).

---

## Board HTTP API

| Endpoint | Purpose |
|---|---|
| `/` | Self-describing web page (tools render dynamically from the store) |
| `/status` | `{tool, store_url, loaded, wifi, provisioning, ip}` |
| `/state` | Live VM slots (`name, type, value`) + render primitives |
| `/schema` | The VM vocabulary — what an LLM needs to author a behavior |
| `/load?id=<tool>` | Compile + run a behavior from the store |
| `/store?url=...` | Point the board at a behavior store |
| `/wifi?ssid=&pass=` | Join home WiFi (persisted in NVS; never in the repo) |

Captive-portal probes (`/generate_204`, `/hotspot-detect.html`, Apple/Google/
Microsoft hosts) are intercepted with a 302 to the setup page when the board is
in provisioning mode (no saved WiFi).

---

## Tooling

### Author a tool with an LLM
```bash
python3 tools/author.py "I want a dice roller that rolls 1-6" --board 192.168.1.206
```
The pipeline: LLM writes JSON against the board's vocabulary → validator →
bounded self-correction loop → `behaviors/<id>.json` → pushed live to the board.

> **Provider note:** the OpenRouter DeepSeek-V4-Flash calls must include
> `"reasoning": {"enabled": false}` or the model burns its whole token budget
> on invisible reasoning and returns `content: None`. `author.py` handles this.

### Validate a behavior offline
```bash
python3 tools/validate_behavior.py behaviors/dice.json
```

### View the board's screen in any browser
Open `tools/board_view.html` (or `http://<board>/`) — it renders the screen from
`/schema` + the behavior JSON + `/state`, with live state and simulate-tap.

### Control from Telegram
Message `@OneBoardControl_bot`: `/status`, `/tools`, `/load <id>`, `/schema`, `/help`.
Runs as the systemd user service `oneboard-tg-bot` (see `systemd/`).
Token lives in `~/.hermes/keys/oneboard-telegram.env` (0600) — never in the repo.

### Run the store
```bash
python3 store/serve.py --port 8082
```
(8082 is used here because 8080 is occupied by a local docker-proxy.)

---

## Built-in behaviors

`behaviors/` — the six shipped tools, all loadable over the wire from the store:

`timer` · `temp-logger` · `reaction-game` · `tap-counter` · `stopwatch` · `dice`

All six are also embedded in firmware as PROGMEM fallbacks (offline boot).

---

## Project history / stages

| Stage | What shipped |
|---|---|
| 1 | Web-toggled demo — display, touch, state on hardware |
| 2 | Behavior store — tools as data, `/store` + `/load` over WiFi |
| 3 | Bytecode VM — JSON→bytecode compiled at load, no ArduinoJson on the hot path |
| 4 | Turnkey authoring — `author.py`, validator, `/schema`, `random` verb |
| 5 | Captive-portal provisioning — no serial, no BOOT button |
| 6 | Telegram control — thin no-LLM bot, systemd service |
| 7 | Any-client rendering — dynamic web page, `/state`, `board_view.html` |

---

## Dev / build

```bash
pio run                    # build
pio run -t upload          # flash over USB (/dev/ttyUSB0)
pio run -t monitor         # serial log, 115200 baud
```

WiFi password is stored only in the board's NVS — never committed, never asked
for in chat.

---

## License

MIT — see [LICENSE](LICENSE).
