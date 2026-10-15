#include <Arduino.h>
#include <TFT_eSPI.h>

#define TFT_BL 21

TFT_eSPI tft = TFT_eSPI();

void setup() {
  // backlight on
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // init TFT in landscape
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Stage 1 ready", 10, 10, 4);
}

void loop() {
  delay(100);
}