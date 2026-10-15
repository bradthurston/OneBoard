# OneBoard — supported CYD boards

The firmware is one codebase; what changes between boards is the display
driver + pins and the touch topology, all set in `platformio.ini` build flags.
This page is the pin map and the honest verification status of each board.

> **Bottom line:** `esp32-2432s028` is the only env HARDWARE-VERIFIED here.
> The S024 and S032 envs are built from vendor schematics and battle-tested
> community configs, but nobody has flashed this firmware on one yet — treat
> them as "should work, prove it on your board."

---

## Pin table

| | **2432S028R** (2.8") | **2432S024** (2.4") | **2432S032** (3.2") |
|---|---|---|---|
| Display driver | ILI9341 | ILI9341 | ILI9341 **or** ST7789 (varies) |
| Resolution | 240×320 | 240×320 | 240×320 |
| SPI port | HSPI | HSPI | HSPI |
| TFT_MISO | 12 | 12 | 12 (ILI9341) / n/a (ST7789) |
| TFT_MOSI | 13 | 13 | 13 |
| TFT_SCLK | 14 | 14 | 14 |
| TFT_CS | 15 | 15 | 15 |
| TFT_DC | 2 | 2 | 2 |
| TFT_RST | -1 | -1 | -1 |
| **TFT_BL (backlight)** | **21** | **27** | 21 or 27 (varies) |
| **Touch topology** | **separate VSPI** | **shared display bus** | shared display bus |
| Touch CS | 33 | 33 | 33 |
| Touch CLK | 25 | (uses TFT_SCLK 14) | (uses TFT_SCLK 14) |
| Touch MOSI | 32 | (uses TFT_MOSI 13) | (uses TFT_MOSI 13) |
| Touch MISO | 39 | (uses TFT_MISO 12) | (uses TFT_MISO 12) |
| Touch IRQ | 36 | — | — |

The **whole family shares the display wiring** (ILI9341 on HSPI
12/13/14/15/2, RST-1). The differences are:

1. **Backlight pin** — S028=21, S024=27, S032 varies (21 or 27).
2. **Touch bus** — the S028 has the XPT2046 on its **own VSPI bus**
   (25/32/39/33/36). The S024 and S032 put the XPT2046 on the **same SPI bus
   as the display**, sharing MISO/MOSI/SCLK with a separate CS (33).
3. **Panel IC on the S032** — ships as ILI9341 *or* ST7789 depending on batch.

---

## Choosing your env

```bash
pio run -e esp32-2432s028             # bench board (verified)
pio run -e esp32-2432s024             # 2.4" GUITION / Sunton
pio run -e esp32-2432s032-ili9341     # 3.2" with ILI9341 panel
pio run -e esp32-2432s032-st7789      # 3.2" with ST7789 panel
pio run -e esp32-2432s032-st7789 -t upload   # + flash
```

### How to tell which S032 panel you have
- **ILI9341** usually reads back a manufacturer ID via the display and shows
  full 240×320 with the ILI9341 flags.
- **ST7789** often needs `TFT_RGB_ORDER=TFT_BGR` and `TFT_INVERSION_ON`
  (already in that env) or colors look wrong/inverted.
- Easiest test: flash the ILI9341 env. If you get a lit-but-wrong screen
  (garbage colors / odd offset), flash the ST7789 env. One of them will work.
- If you can see the LCD driver IC under the ribbon, read its marking.

---

## Verification status

| Env | Source of pins | Hardware-verified? |
|---|---|---|
| `esp32-2432s028` | Bench board, live hardware (this repo) | **YES** |
| `esp32-2432s024` | Vendor NoosaHydro `User_Setup.h` + marauder `User_Setup_cyd_guition.h` (both agree) | No — test on real board |
| `esp32-2432s032-*` | Vendor kpeeem PDQ demo + community; sub-variants documented | No — test on real board |

What "verify" means here: the board boots, the display renders the
behavior tools, and touch reports presses. The S024/S032 envs are *expected*
to work from the schematics, but the CYD world is full of silent pin-map
differences between factories — if you have one of these boards, flash the
matching env and report back so this table can be upgraded to verified.

### Not (yet) supported
- **ESP32-3248S035 (3.5", 320×480 ILI9486)** — different resolution and a
  larger framebuffer; the display driver is ILI9486 (not ILI9341) and the
  touch shares the bus. Needs a real port (resolution + driver + rotation),
  not just pin flags. Out of scope for now.
- **Capacitive-touch variants** (e.g. 2432S024C / 2432S032C) — those use a
  GT911 (I2C) touch controller instead of the resistive XPT2046. Different
  touch driver entirely. Not supported yet.

---

## How to add a new board

1. Add an env to `platformio.ini` extending `[common]`.
2. Set the display driver (`-DILI9341_DRIVER` / `-DST7789_DRIVER` / ...),
   the HSPI pins, `-DTFT_BL`, and `-DUSE_HSPI_PORT`.
3. Set the touch: `-DTOUCH_SHARED_BUS=1` (shared display bus, set `TOUCH_CS`)
   or `0` (separate VSPI, set all five pins).
4. `pio run -e <env>` — it must compile.
5. Flash it on the real board and confirm display + touch work.
6. Move it from "No" to "YES" in the verification table above.
