# TOUCH CONSULT — E32R28T CYD touch not responding (paste to GPT)

**Follow-up to your earlier pin-map fix** (which worked: display now renders correctly).

**Working state now:**
- E32R28T exposed-SPI CYD, ILI9341 240x320, TFT_eSPI 2.5.43.
- TFT renders perfectly with your pin map: MISO 12, MOSI 13, SCLK 14, CS 15, DC 2,
  RST tied to EN (TFT_RST=-1), backlight 21. SPI 20MHz. Fonts compiled in (LOAD_*).
- Firmware boots clean, timer behavior runs and counts (state-write bug fixed).

**The problem:** touch does nothing. `tft.getTouch()` never reports a touch even with
active tapping — zero events in 15s.

**My suspicion:** my PlatformIO build_flags still carry the VANILLA CYD touch CS:
`TOUCH_CS=14`. But on this E32R28T, **GPIO 14 is SCLK** (per your own TFT fix). The touch
controller (XPT2046) shares the SPI bus with the display, so its CS must be a DIFFERENT
GPIO that doesn't collide with 12/13/14 (or 15/2/21). Pin 14 can't be both SCLK and
TOUCH_CS.

**Question:**
1. What is the correct **TOUCH_CS GPIO** for the E32R28T exposed-SPI CYD? (Not the vanilla
   2432S028's pin 14 — that collides.)
2. Any other touch wiring needed: does the touch share the same HSPI (12/13/14) as the
   display on this variant, or does it need its own SPI instance? Confirm the exact
   TFT_eSPI build flags for a working touch on this board.
3. Also confirm: on this variant, are touch coordinates landscape (320x240) after
   `tft.setRotation(1)`, matching the display orientation?

Give exact, correct-before-creative answers — I compile and flash, I can't eyeball
traces.