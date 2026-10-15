# Touch hardware check — XPT2046 QFN-16 (E32R28T / HaleHound)

GPT photo-verified the XPT2046 touch controller is POPULATED. Not a no-touch variant.
The stock-library diagnostic already failed (IRQ high, z=0). Next: verify power + continuity.

## XPT2046 QFN-16 pinout (authoritative, from datasheet)

Layout (pin 1 marked with dot; pin 1 = BUSY):

```
  pin 1  BUSY      pin 9   YN
  pin 2  DIN       pin 10  GND
  pin 3  CS        pin 11  VBAT
  pin 4  DCLK      pin 12  AUX
  pin 5  VCC       pin 13  VREF
  pin 6  XP        pin 14  IOVDD
  pin 7  YP        pin 15  PENIRQ
  pin 8  XN        pin 16  DOUT
```

## Signal → ESP32 GPIO map (documented for E32R28T)

| XPT2046 pin | XPT2046 name | ESP32 GPIO | role                |
|-------------|-------------|------------|---------------------|
| 2           | DIN         | 32         | data in  (MOSI)     |
| 16          | DOUT        | 39         | data out (MISO)     |
| 4           | DCLK        | 25         | serial clock (SCLK) |
| 3           | CS          | 33         | chip select         |
| 15          | PENIRQ      | 36         | pen interrupt       |
| 5           | VCC         | —          | power               |
| 10          | GND         | —          | ground              |
| 14          | IOVDD       | —          | digital I/O rail    |

## Ordered checks (do NOT probe an unidentified pin first)

1. **Confirm the real pin 1** — the dot on the package. Do not guess orientation.
2. **Power (board powered):** VCC (pin 5) to GND (pin 10) should read ~3.3V. IOVDD (pin 14) should
   also be ~3.3V (or ≥ VCC level). If absent → power feed problem.
3. **Continuity (power OFF):** ESP32 GPIO 25/32/39 → XPT2046 DCLK/DIN/DOUT; GPIO 33 → CS;
   GPIO 36 → PENIRQ. Each should be a short (< ~10Ω). Broken trace / cold joint / unseated flex
   shows as open.
4. **Pen interrupt live (power on, controller idle):** measure PENIRQ (pin 15) at the CHIP while
   pressing the panel. If it asserts low on press but GPIO 36 stays high → investigate between-pin
   connection. If it never changes → panel/controller side.
5. If power + continuity + PENIRQ all check out but GPIO 36 never sees it → the ESP32 side or the
   connection GPIO36↔pin15 is the fault.

## Safety
- Power OFF for continuity. Power ON only for voltage / live signal reads.
- Identify package + pin 1 before any probe. Capacitors and the panel flex look similar.
- GPIO 36 (input-only) has no internal pull-up — do not measure expecting a default level.

## Decision guide (matches GPT)
- All checks pass but still z=0/IRQ high → revisit driver/bus init (software), not hardware.
- Power absent or trace open at that pin → physical repair (resolder / reseat flex).