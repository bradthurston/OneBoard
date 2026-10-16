# OneBoard — Installation & Setup

End-to-end guide: from a bare machine to a live, controllable board.

**Time:** ~20 minutes first time (mostly platform downloads).

---

## 1. Hardware you need

- A **CYD ESP32-2432S028(R)** board ("Cheap Yellow Display", 240×320)
- A **USB data cable** (micro-USB; power-only cables won't flash)
- A computer with USB (Linux, macOS, or Windows)
- A **WiFi network** your board can join

> The repo targets the 2432S028. If your board has a different display
> controller, check `platformio.ini` and the docs before flashing — the CYD
> pin map is display-specific.

---

## 2. Linux one-shot installer (fast path)

On a Debian/Ubuntu, Fedora, or Arch machine you can skip most of the manual
setup. Clone the repo and run:

```bash
git clone https://github.com/bradthurston/OneBoard.git
cd OneBoard
./install.sh
```

What it does:
- Installs system packages (git, curl, python3, venv, pip) via apt/dnf/pacman
- Installs PlatformIO (official standalone installer, pip as fallback)
- Adds your user to the **dialout** group so USB flashing works (log out/in
  after)
- Creates the project `.venv` and installs `requirements.txt`
- Verifies `python-telegram-bot` imports from a clean env (catches the
  leaked-PYTHONPATH trap before it bites under systemd)

Options:
```bash
./install.sh --no-system      # skip distro package install (you did it yourself)
./install.sh --with-store     # also install the behavior store as a systemd user service (port 8082)
./install.sh --with-telegram  # also install the Telegram bot as a systemd user service
```

`--with-store` runs the store permanently as `oneboard-store.service`
(survives reboot). `--with-telegram` creates
`~/.hermes/keys/oneboard-telegram.env` from the template if it doesn't exist —
fill in the token, then re-run with `--with-telegram` to start the bot.

Idempotent — safe to re-run any time. Continue at **step 6** (first boot /
captive portal) after flashing.

---

## 3. Install PlatformIO (manual)

> The fast path above already did this — jump to step 6 after flashing.

PlatformIO is the build/flash toolchain.

**Linux / macOS:**
```bash
pip install --user platformio
# or via the standalone installer:
# curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core-installer/master/get-platformio.py | python3 -
```
**Windows:** install [VSCode](https://code.visualstudio.com) + the PlatformIO IDE extension, then use the PlatformIO terminal below.

Verify:
```bash
pio --version
```

---

## 4. Get the code

```bash
git clone https://github.com/bradthurston/OneBoard.git
cd OneBoard
```

---

## 5. Flash the firmware

Plug the board in and find its serial port:
- Linux: `/dev/ttyUSB0` (check `ls /dev/ttyUSB*`)
- macOS: `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial*`
- Windows: `COM3` etc (Device Manager → Ports)

Build and flash:
```bash
pio run -t upload --upload-port /dev/ttyUSB0     # adjust port for your OS
```

> **Which board do you have?** The default env is the 2.8" 2432S028R. If your
> board is a 2432S024 (2.4") or 2432S032 (3.2"), pick the matching env first:
> `pio run -e esp32-2432s024 -t upload` (or `-e esp32-2432s032-ili9341` /
> `-e esp32-2432s032-st7789`). Full pin table + verification status:
> **[VARIANTS.md](VARIANTS.md)**.

Watch it boot (optional, 115200 baud):
```bash
pio run -t monitor
```

You should see `OneBoard Stage 3 ready` and the board's AP appear.

---

## 6. First boot — join it to WiFi (captive portal)

1. Power the board (USB is fine). It boots into **provisioning mode** — it
   creates its own WiFi access point.
2. On your phone: **Settings → WiFi → join `OneBoard-Stage3`** (no password).
3. The captive-portal page **auto-opens** (if not, browse to `http://192.168.4.1`).
4. Your home network is already listed — the board scans and shows the nearby
   2.4 GHz networks with signal strength, strongest first. **Tap yours** to fill
   the name, then type the password and tap **Join Home WiFi**. (Hidden networks
   can't be listed, so the text field is still there if you need it.)
5. The board joins your network and shows its new IP on screen (also visible
   at `/status`).

> **2.4 GHz only.** The ESP32 has no 5 GHz radio, so only 2.4 GHz networks appear
> in the list — and those are the only ones the board can join anyway. If your
> router publishes one name for both bands, you'll see it once.

From now on the board remembers the WiFi in NVS — it auto-joins on every boot.

> No serial cable, no BOOT button, ever again. The captive portal intercepts
> the standard phone probes (Apple/Google/Microsoft) so the page just opens.

---

## 7. Point the board at a behavior store

Tools ("behaviors") are JSON files served from a machine on your network —
the board loads them over WiFi, compiles them to bytecode, and runs them.

Start the store on a machine that stays on (your desktop, a VPS, a Pi):

```bash
cd OneBoard
python3 store/serve.py --port 8082
```

Then the board finds it by itself — nothing to type. `serve.py` advertises
itself over mDNS as `_oneboard-store._tcp.local` and the board browses for that
on every boot.

```bash
python3 store/serve.py --port 8082
#   OneBoard Behavior Store @ http://0.0.0.0:8082/
#   mDNS: advertising _oneboard-store._tcp.local
#         boards will find it at http://192.168.1.169:8082/
```

That last line tells you the address the boards will use. Look-up happens fresh
each boot, so if the store machine's address ever changes the board just finds
it again — nothing to re-configure.

Verify: browse to `http://<board-ip>/` — the **Tools** list should populate from
the store on its own, and **Store** should read "Found automatically".

**If it doesn't find it** (store on a different subnet, or mDNS blocked on your
network), set it by hand: web page → **Store URL (manual override)**. A manual
URL switches automatic discovery off, because an address you typed is a
deliberate choice; **Find store automatically** switches it back on. You can also
run `--no-mdns` if you'd rather a store stay unadvertised.

`python3 store/mdns_advertise.py --port 8082` advertising on its own is handy for
testing: `avahi-browse -rt _oneboard-store._tcp` should print the record.

---

## 8. Author tools with an LLM (optional but the point of the project)

`tools/author.py` turns a plain-English request into a working tool on the
board. It needs an OpenRouter API key:

1. Get a key at https://openrouter.ai (a few dollars of credit is plenty).
2. Store it where the tool can read it:
   ```bash
   echo "sk-or-v1-..." > ~/.openrouter_key
   chmod 600 ~/.openrouter_key
   ```
3. Author your first tool:
   ```bash
   cd OneBoard
   python3 tools/validate_behavior.py examples/store-only/dice.json   # sanity check first
   OPENROUTER_KEY_FILE=~/.openrouter_key \
     python3 tools/author.py "I want a dice roller that rolls 1-6" --board 192.168.1.206
   ```

The pipeline: LLM writes JSON against the board's vocabulary → validated →
self-corrected on errors → saved to `behaviors/` → pushed live to the board
via `/store` + `/load`. No reflash.

> The store ships exactly the four tools the firmware comes with, so a fresh
> store is never cluttered: timer, temp-logger, reaction-game, tap-counter.
> Four more worked examples live in `examples/store-only/` (dice, stopwatch,
> multiplication-f, two-buttons) — validated by CI, but not served, because a
> store should not advertise tools a from-scratch board doesn't have.

---

## 9. Control from Telegram (optional)

1. In Telegram, message **@BotFather** → `/newbot` → pick a name and username.
   Copy the token it gives you.
2. Create the bot's env file (never commit it):
   ```bash
   cp systemd/oneboard-telegram.env.template ~/.hermes/keys/oneboard-telegram.env
   # fill in: ONBOARD_BOT_TOKEN=<your token>
   #          ONBOARD_BOARD_IP=<board ip>
   #          ONBOARD_STORE_URL=http://<store-machine>:8082
   #          ONBOARD_ALLOWED=<your telegram numeric user id>
   ```
3. Set up the Python venv and install the bot's only dependency:
   ```bash
   cd OneBoard
   python3 -m venv .venv
   .venv/bin/pip install python-telegram-bot
   ```
4. Install + start the service (the installer generates the unit with the
   correct repo path — don't hand-copy a service file):
   ```bash
   ./install.sh --with-telegram
   ```
   or, to do it by hand:
   ```bash
   mkdir -p ~/.config/systemd/user
   cat > ~/.config/systemd/user/oneboard-tg-bot.service <<EOF
   [Unit]
   Description=OneBoard Telegram control bot
   After=network-online.target
   [Service]
   Type=simple
   WorkingDirectory=$PWD
   EnvironmentFile=$HOME/.hermes/keys/oneboard-telegram.env
   ExecStart=$PWD/.venv/bin/python $PWD/tools/oneboard_bot.py
   Restart=on-failure
   RestartSec=5
   [Install]
   WantedBy=default.target
   EOF
   systemctl --user daemon-reload
   systemctl --user enable --now oneboard-tg-bot
   ```
5. In Telegram, message your bot: `/status`, `/tools`, `/load timer`, `/schema`, `/help`.

> The env file (`oneboard-telegram.env`) must contain `ONBOARD_BOT_TOKEN`,
> `ONBOARD_BOARD_IP`, `ONBOARD_STORE_URL`, and `ONBOARD_ALLOWED` (your Telegram
> numeric user id) or the bot refuses to start.

---

## 10. View the screen from any browser

Open `tools/board_view.html` in a browser, enter the board's IP, and it
renders the board's actual screen from `/schema` + the behavior + `/state` —
with live state and a "simulate tap" button.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| White screen / 4-digit number on boot | Wrong display driver config — this board needs `ILI9341_2_DRIVER`, `USE_HSPI_PORT`, fonts 2/4/6 (see `platformio.ini`). Don't "simplify" it. |
| Touch does nothing | Touch is on separate VSPI; `USE_HSPI_PORT` is **mandatory** or touch is silently dead. |
| `/load` fails on the board | Is `store/serve.py` running? Is the store URL set correctly on the board? Test `curl http://<store>:8082/` from the same network. |
| `ModuleNotFoundError: No module named 'telegram'` | Run the bot with `.venv/bin/python`, not system python (step 9.3). |
| Author tool returns empty / stalls | DeepSeek on OpenRouter can burn its budget on invisible reasoning — the tool already sends `reasoning:{enabled:false}`; make sure your key has credit. |
| Captive portal page doesn't auto-open | Browse manually to `http://192.168.4.1` after joining the AP. |
| **Store tool list is empty / "store isn't loading"** | The board finds the store over mDNS by itself (`_oneboard-store._tcp.local`), so first check that `serve.py` printed `mDNS: advertising...` at startup — `--no-mdns`, a store on another subnet, or a network that filters multicast will all stop discovery. If discovery can't work on your network, set the URL by hand: web page → **Store URL (manual override)**, or `curl -G "http://<board-ip>/store" --data-urlencode "url=http://<your-pc-ip>:8082"`. Note: a manual URL **switches automatic discovery off** (an address you typed should win), and **Find store automatically** turns it back on. `/status` reports `store_auto` and `store_found` so you can see which is in play. Also check the port matches (`--port 8082` here, because 8080 was taken). |

---

## Quick reference

```
Board IP (AP) : 192.168.4.1         Board IP (WiFi): shown on screen / /status
Store port    : 8082 (8080 often taken by other services)
Serial        : /dev/ttyUSB0 @ 115200
Key file      : OPENROUTER_KEY_FILE (author tool)
Bot token     : ~/.hermes/keys/oneboard-telegram.env (0600)
WiFi creds    : board NVS only — never in the repo, never in chat
```

See [docs/BEHAVIOR-AUTHORING.md](docs/BEHAVIOR-AUTHORING.md) for the full
behavior authoring reference, and the [README](README.md) for architecture.
