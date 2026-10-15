# TOUCH CONSULT v2 — E32R28T touch dead, measured evidence (paste to GPT)

**Follow-up 2. Your earlier fixes worked:** E32R28T exposed-SPI CYD, ILI9341, display renders
perfectly in **portrait rotation 0** (240x320) using TFT HSPI pins MISO12/MOSI13/SCLK14, CS15, DC2,
RST=-1, BL21. Timer counts. Touch is the only thing dead.

**What I implemented from your v1 guidance (separate touch SPI instance):**
- Removed TFT_eSPI's built-in touch (TOUCH_CS no longer in build flags).
- Added my own XPT2046 driver on a **separate SPIClass(VSPI)** with manufacturer pinout:
  SCLK=25, MOSI=32, MISO=39, CS=33, IRQ=36. SPI mode 0, 2.5MHz.
- Used the same raw sampling protocol as TFT_eSPI's Touch.cpp (0xd0/0x90 commands, Z pressure via 0xb0).
- Edge-detected one-event-per-press in the main loop.

**Measured results (live serial during active tapping):**
- **Z pressure reads are pure noise: max 141, min 4, threshold 350 — never crosses.** No real pressure ever registers.
- **IRQ pin (GPIO 36) NEVER goes low when the screen is pressed.** XPT2046 IRQ should assert low on pen-down. It stayed high through many taps.

**So my read is:** either the touch controller's IRQ/CS isn't on the pins I assumed, or the touch controller on THIS board isn't responding on that bus at all. I don't want to keep guessing pins — give me a **verified instruction set**.

**Please give me:**
1. **Exact, correct pin table** for the E32R28T exposed-SPI variant touch controller — IRQ, CS, SCLK, MOSI, MISO as GPIO numbers. (My paste earlier got truncated right at your pin table, so I never saw the full table — that's the #1 thing I need.)
2. Whether this variant's touch IRQ is actually connected/usable, or a known quirk (e.g. IRQ not wired, needs pull-up, different GPIO).
3. A **minimal verified test sketch** for just the touch: init the SPI, read Z + raw X/Y, print to serial, and if pressed print "PRESSED". I'll flash it verbatim and report exactly what it prints — then we know the true pinout empirically before touching the real firmware.
4. If the E32R28T exposed-SPI variant simply has NO touch controller populated (it's a radio-module breakout board), say so plainly — then I'll know touch is a hardware dead-end on this board and stop chasing it.

Exact-before-creative. I compile and flash; I can't see the board.