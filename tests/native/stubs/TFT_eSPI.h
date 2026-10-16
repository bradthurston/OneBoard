#pragma once
#include <cstdint>
enum { TFT_BLACK, TFT_WHITE, TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW,
       TFT_CYAN, TFT_MAGENTA, TFT_ORANGE, TFT_NAVY, TFT_DARKGREY,
       MC_DATUM, TL_DATUM };
class TFT_eSPI {
public:
  // Draw counters, so tests can assert *what was painted* — notably that a
  // self-erasing text blanks its own small box instead of clearing the screen.
  static inline int fillScreen_calls = 0;
  static inline int fillRect_calls = 0;
  static inline int16_t last_fr_x = 0, last_fr_y = 0, last_fr_w = 0, last_fr_h = 0;
  static void resetCalls() {
    fillScreen_calls = fillRect_calls = 0;
    last_fr_x = last_fr_y = last_fr_w = last_fr_h = 0;
  }

  void fillScreen(uint16_t) { ++fillScreen_calls; }
  void setTextColor(uint16_t) {}
  void setTextDatum(uint8_t) {}
  void drawString(const char*, int16_t, int16_t, uint8_t) {}
  void drawRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t) {
    ++fillRect_calls; last_fr_x = x; last_fr_y = y; last_fr_w = w; last_fr_h = h;
  }
  void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void fillCircle(int16_t, int16_t, int16_t, uint16_t) {}

  // GLCD metrics, matching TFT_eSPI's built-in fonts closely enough for the host
  // harness (1 = 6x8, 2 = 8x16, 4 = 16x26, 6 = 32x48). The VM uses these only to
  // size a self-erasing text box, and the regression test asserts on control
  // flow rather than pixels — but they must exist, or the native build breaks.
  static int16_t glcdW(uint8_t font) {
    switch (font) {
      case 1: return 6;  case 2: return 8;  case 3: return 12;
      case 4: return 16; case 5: return 24; case 6: return 32;
      case 7: return 48; default: return 8;
    }
  }
  static int16_t glcdH(uint8_t font) {
    switch (font) {
      case 1: return 8;  case 2: return 16; case 3: return 24;
      case 4: return 26; case 5: return 32; case 6: return 48;
      case 7: return 56; default: return 16;
    }
  }
  int16_t textWidth(const char* s, uint8_t font) {
    int16_t n = 0;
    for (const char* p = s; p && *p; ++p) ++n;
    return (int16_t)(n * glcdW(font));
  }
  int16_t fontHeight(int16_t font) { return glcdH((uint8_t)font); }
  int16_t width() const { return 240; }
  int16_t height() const { return 320; }
};
