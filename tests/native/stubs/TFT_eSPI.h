#pragma once
#include <cstdint>
enum { TFT_BLACK, TFT_WHITE, TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW,
       TFT_CYAN, TFT_MAGENTA, TFT_ORANGE, TFT_NAVY, TFT_DARKGREY,
       MC_DATUM, TL_DATUM };
class TFT_eSPI {
public:
  void fillScreen(uint16_t) {}
  void setTextColor(uint16_t) {}
  void setTextDatum(uint8_t) {}
  void drawString(const char*, int16_t, int16_t, uint8_t) {}
  void drawRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void fillRect(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void drawLine(int16_t, int16_t, int16_t, int16_t, uint16_t) {}
  void fillCircle(int16_t, int16_t, int16_t, uint16_t) {}
};
