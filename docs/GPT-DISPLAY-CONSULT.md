# DISPLAY CONSULT — CYD blank/white screen (paste to GPT)

**Symptom:** ESP32 CYD "Cheap Yellow Display" board is running fine (serial boots clean, code
executes, web AP up) but the 2.8" TFT stays **blank/white**. A diagnostic that fills solid
RED/GREEN/BLUE (4s each) also shows **nothing — pure white**. When the 1Hz timer tick rendered, the
screen visibly "flashed" each tick (so SPI IS driving something) but no image data ever appeared.

**Board: confirmed E32R28T exposed-SPI variant** (not vanilla 2432S028). CH340 USB-serial (`1a86:7523`). This variant has a breakout connector on the SPI bus — the TFT pin map is almost certainly different from vanilla CYD. **This is the root cause of the white screen** — my pin config routes writes to the wrong display-control pins.

**Framework:** PlatformIO, espressif32, Arduino, TFT_eSPI 2.5.43.

**What I've tried (all blank):**
1. ILI9341 driver + vanilla CYD pins (CS15/DC2/RST4/BL21/MISO19/MOSI23/SCLK18, TOUCH_CS14), 27MHz
2. Same + `tft.invertDisplay(true)`
3. SPI lowered to 20MHz
4. ST7789 driver + `COLOR_ORDER=BGR`, same pins, 20MHz

**Key clues:**
- CPU runs (interpreted behavior ticks, serial works, AP network up) → not a boot/hang problem.
- The tick's periodic redraw makes the white screen "flash" → the panel is receiving *some* activity.
- A minimal init+fillScreen(RED/GREEN/BLUE) diag also shows white → not my renderer or behavior, it's
  the display init/driver/pins layer.
- The user has flashed other CYD firmware that DID display but with **inverted colors** → the panel
  itself works; this seems to be a driver/pin/init vs the specific panel variant problem.

**Questions for you:**
1. What is the correct TFT_eSPI `USER_SETUP_LOADED`/build-flag pin map + driver for the **E32R28T
   exposed-SPI** CYD vs the **vanilla 2432S028**? Tell me the exact defines for each.
2. Is there a known init quirk (bus/mode/reset line) where the panel runs-but-stays-white even with
   the right driver? Common cause + the one-line fix in USER_SETUP.
3. Best way to identify which panel variant I actually have from software or the board (read IDs?
   which pins to probe? what marks on the PCB?) so we stop guessing the pin map.

Give me exact, correct-before-creative guidance — I compile/flash, I can't eyeball prints.