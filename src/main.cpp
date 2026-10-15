#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

/* ==========================================================
   Configuration
   ========================================================== */
#define TFT_BL          21

#define SOFT_AP_SSID    "OneBoard-Stage1"
#define SOFT_AP_PSK     ""              // open AP

#define DEMO_NS         "obm_demo"
#define DEMO_KEY        "active"

/* ==========================================================
   Globals
   ========================================================== */
TFT_eSPI   tft = TFT_eSPI();
WebServer  server(80);
Preferences prefs;

// Active demo selector (stored as char for NVS convenience)
char       activeDemo[16] = "timer";   // "timer", "temp", "reaction"
bool       demoChanged    = true;      // force first draw

/* ==========================================================
   Timer demo state
   ========================================================== */
unsigned long timerStartMs = 0;
unsigned long timerPrevSec = 99;       // force redraw on first tick

/* ==========================================================
   Temp logger demo state
   ========================================================== */
#define TEMP_HISTORY_SIZE  100
float     tempHistory[TEMP_HISTORY_SIZE];
int       tempHistoryIdx     = 0;
int       tempHistoryCount   = 0;
float     tempCurrent        = 72.0f;
unsigned long tempLastTick   = 0;

/* ==========================================================
   Reaction game demo state
   ========================================================== */
enum ReactionPhase {
  REACT_INACTIVE,
  REACT_WAITING,
  REACT_GO,
  REACT_SHOW_TIME
};
ReactionPhase reactPhase = REACT_INACTIVE;
unsigned long reactWaitStart  = 0;     // millis() when waiting began
unsigned long reactGoAt       = 0;     // millis() when GO appeared
unsigned long reactDelayMs    = 0;     // random wait between 1-5s
uint16_t      reactLastX      = 0;
uint16_t      reactLastY      = 0;
unsigned long reactTouchTime  = 0;
unsigned long reactShowUntil  = 0;     // millis() to show result

/* ==========================================================
   Drawing helpers
   ========================================================== */
void drawCentered(const char* text, int y, int font, uint16_t color) {
  tft.setTextColor(color);
  tft.drawCentreString(text, 120, y, font);
}

void drawStatus(const char* demoName) {
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawRightString(demoName, 238, 0, 2);   // top-right corner
}

void enterDemo(const char* name) {
  tft.fillScreen(TFT_BLACK);
  drawStatus(name);
}

/* ==========================================================
   Demo: Timer — big MM:SS count-up
   ========================================================== */
void demo_timer_setup() {
  enterDemo("Timer");
  timerStartMs = millis();
  timerPrevSec = 99;
}

void demo_timer_loop() {
  unsigned long elapsed = (millis() - timerStartMs) / 1000;
  if (elapsed == timerPrevSec) return;
  timerPrevSec = elapsed;

  unsigned long mm = elapsed / 60;
  unsigned long ss = elapsed % 60;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", mm, ss);

  // Clear previous digits area
  tft.fillRect(0, 80, 240, 80, TFT_BLACK);
  drawCentered(buf, 120, 6, TFT_GREEN);

  // progress bar
  unsigned long maxSec = 3600; // 1h wrap
  int barW = (elapsed % maxSec) * 220 / maxSec;
  tft.fillRect(10, 170, barW, 8, TFT_BLUE);
  tft.fillRect(10 + barW, 170, 220 - barW, 8, TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Count-up since demo start", 10, 200, 2);
}

/* ==========================================================
   Demo: Temp Logger — simulated sensor + rolling graph
   ========================================================== */
void demo_temp_setup() {
  enterDemo("Temp Logger");
  tempLastTick = 0;
}

void demo_temp_loop() {
  unsigned long now = millis();
  if (now - tempLastTick < 250) return;
  tempLastTick = now;

  // Random walk around 72°F
  tempCurrent += ((random(100) - 50) * 0.04f);
  if (tempCurrent < 60.0f) tempCurrent = 60.0f;
  if (tempCurrent > 90.0f) tempCurrent = 90.0f;

  if (tempHistoryCount < TEMP_HISTORY_SIZE) {
    tempHistory[tempHistoryIdx] = tempCurrent;
    tempHistoryIdx = (tempHistoryIdx + 1) % TEMP_HISTORY_SIZE;
    tempHistoryCount++;
  } else {
    tempHistory[tempHistoryIdx] = tempCurrent;
    tempHistoryIdx = (tempHistoryIdx + 1) % TEMP_HISTORY_SIZE;
  }

  // --- draw ---
  // Clear the graph area (bottom 2/3) and title zone
  tft.fillRect(0, 24, 240, 296, TFT_BLACK);
  drawStatus("Temp Logger");

  // Current temp
  char tempBuf[24];
  snprintf(tempBuf, sizeof(tempBuf), "%.1f°F", tempCurrent);
  drawCentered(tempBuf, 50, 4, TFT_CYAN);

  // Graph area: map temp [60,90] → y [240, 60]
  int     n = tempHistoryCount;
  int     graphTop  = 70;
  int     graphBot  = 280;
  int     graphW    = (n > 1) ? (200 / (n - 1)) : 1;

  tft.drawRect(10, graphTop, 220, graphBot - graphTop, TFT_DARKGREY);

  // Draw min/max labels
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("90°", 215, graphTop, 1);     // right-aligned
  tft.drawString("60°", 215, graphBot - 10, 1);

  for (int i = 0; i < n - 1; i++) {
    int idx  = (tempHistoryIdx - n + i + TEMP_HISTORY_SIZE) % TEMP_HISTORY_SIZE;
    int idx1 = (idx + 1) % TEMP_HISTORY_SIZE;

    float v0   = tempHistory[idx];
    float v1   = tempHistory[idx1];
    int   y0   = graphBot - ((v0 - 60.0f) / 30.0f) * (graphBot - graphTop);
    int   y1   = graphBot - ((v1 - 60.0f) / 30.0f) * (graphBot - graphTop);
    int   x0   = 10 + i * graphW;
    int   x1   = 10 + (i + 1) * graphW;

    tft.drawLine(x0, y0, x1, y1, TFT_GREEN);
  }
}

/* ==========================================================
   Demo: Reaction Game — touch test
   ========================================================== */
void demo_reaction_setup() {
  enterDemo("Reaction Game");
  reactPhase = REACT_WAITING;
  reactWaitStart = millis();
  reactDelayMs   = 1000 + (random(4001)); // 1-5s
  reactGoAt      = 0;
  reactTouchTime = 0;
  reactShowUntil = 0;

  // Show READY indicator
  tft.fillCircle(120, 140, 50, TFT_GREEN);
  drawCentered("READY...", 200, 2, TFT_WHITE);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("(wait for red)", 70, 232, 2);
}

void demo_reaction_loop() {
  uint16_t tx = 0, ty = 0;
  bool touched = tft.getTouch(&tx, &ty);

  switch (reactPhase) {
    case REACT_INACTIVE:
      // Shouldn't happen, re-init
      demo_reaction_setup();
      break;

    case REACT_WAITING:
      // Ignore touches during waiting phase
      if (millis() - reactWaitStart >= reactDelayMs) {
        // Switch to GO
        reactPhase = REACT_GO;
        reactGoAt  = millis();
        tft.fillScreen(TFT_BLACK);
        drawStatus("Reaction Game");
        tft.fillCircle(120, 140, 50, TFT_RED);
        drawCentered("TAP!", 200, 2, TFT_WHITE);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("Touch the screen!", 40, 234, 2);
      }
      break;

    case REACT_GO:
      if (touched) {
        reactPhase     = REACT_SHOW_TIME;
        reactTouchTime = millis() - reactGoAt;
        reactShowUntil = millis() + 3000;
        reactLastX     = tx;
        reactLastY     = ty;

        // Show reaction time
        tft.fillScreen(TFT_BLACK);
        drawStatus("Reaction Game");
        char buf[24];
        snprintf(buf, sizeof(buf), "%lu ms", reactTouchTime);
        uint16_t color = (reactTouchTime < 300) ? TFT_GREEN :
                        ((reactTouchTime < 600) ? TFT_YELLOW : TFT_RED);
        drawCentered(buf, 120, 6, color);

        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("Touch at (" + String(tx) + "," + String(ty) + ")", 20, 200, 2);
        tft.drawString("Tap screen for next round", 20, 230, 2);
      }
      break;

    case REACT_SHOW_TIME:
      // Auto-advance after 3s, or tap to go again
      if (millis() > reactShowUntil) {
        demo_reaction_setup();
      }
      break;
  }
}

/* ==========================================================
   Demo dispatch
   ========================================================== */
void setActiveDemo(const char* id) {
  if (strcmp(id, "timer") == 0 || strcmp(id, "temp") == 0 || strcmp(id, "reaction") == 0) {
    strncpy(activeDemo, id, 15);
    activeDemo[15] = 0;

    // Persist
    prefs.begin(DEMO_NS, false);
    prefs.putString(DEMO_KEY, activeDemo);
    prefs.end();

    demoChanged = true;
  }
}

void runActiveDemo() {
  if (demoChanged) {
    if (strcmp(activeDemo, "timer") == 0) {
      demo_timer_setup();
    } else if (strcmp(activeDemo, "temp") == 0) {
      demo_temp_setup();
    } else if (strcmp(activeDemo, "reaction") == 0) {
      demo_reaction_setup();
    }
    demoChanged = false;
  }

  if (strcmp(activeDemo, "timer") == 0) {
    demo_timer_loop();
  } else if (strcmp(activeDemo, "temp") == 0) {
    demo_temp_loop();
  } else if (strcmp(activeDemo, "reaction") == 0) {
    demo_reaction_loop();
  }
}

/* ==========================================================
   Web server handlers
   ========================================================== */
void handleRoot() {
  String html = F("<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=320'>"
    "<title>OneBoard Stage 1</title>"
    "<style>"
      "*{box-sizing:border-box;margin:0;padding:0}"
      "body{font-family:-apple-system,Helvetica,sans-serif;background:#111;color:#ddd;text-align:center;padding:20px}"
      "h1{font-size:1.3em;margin-bottom:10px;color:#4af}"
      ".demo-name{font-size:1.1em;margin:12px 0;color:#ff0}"
      "button{display:block;width:100%;max-width:280px;margin:10px auto;padding:14px;"
        "border:2px solid #555;border-radius:10px;background:#222;color:#fff;font-size:1.1em;cursor:pointer}"
      "button:hover{border-color:#4af;background:#333}"
      ".active{background:#4af;color:#000;border-color:#4af}"
      ".footer{margin-top:18px;font-size:0.8em;color:#666}"
    "</style>"
    "</head><body>"
    "<h1>&#9670; OneBoard</h1>"
    "<div class='demo-name' id='current'></div>"
    "<button id='btn-timer' onclick='setDemo(\"timer\")'>&#9201; Timer</button>"
    "<button id='btn-temp'  onclick='setDemo(\"temp\")'>&#127777; Temp Logger</button>"
    "<button id='btn-reaction' onclick='setDemo(\"reaction\")'>&#9889; Reaction Game</button>"
    "<div class='footer'>OneBoard Stage 1 — CYD 2432S028</div>"
    "<script>"
      "async function setDemo(id){"
        "try{"
          "var r=await fetch('/set?demo='+id);"
          "var j=await r.json();"
          "if(j.ok){document.getElementById('current').innerText='Active: '+j.demo}"
        "}catch(e){}"
      "}"
      "fetch('/status').then(r=>r.json()).then(j=>{"
        "document.getElementById('current').innerText='Active: '+j.demo;"
        "var b=document.getElementById('btn-'+j.demo);"
        "if(b)b.className='active'"
      "}).catch(()=>{})"
    "</script></body></html>");

  server.send(200, "text/html", html);
}

void handleSet() {
  if (server.hasArg("demo")) {
    String id = server.arg("demo");
    id.toLowerCase();
    const char* demoStr = id.c_str();

    // Validate
    if (strcmp(demoStr, "timer") == 0 || strcmp(demoStr, "temp") == 0 || strcmp(demoStr, "reaction") == 0) {
      setActiveDemo(demoStr);
      String json = "{\"ok\":true,\"demo\":\"" + id + "\"}";
      server.send(200, "application/json", json);
      return;
    }
  }
  server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid demo\"}");
}

void handleStatus() {
  String json = "{\"demo\":\"" + String(activeDemo) + "\"}";
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "404 Not Found\n");
}

/* ==========================================================
   Setup
   ========================================================== */
void setup() {
  // Serial for debugging (optional)
  Serial.begin(115200);

  // --- Display init ---
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.init();
  tft.setRotation(1);   // landscape
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Stage 1 booting...", 10, 10, 2);

  // --- NVS: load last demo ---
  prefs.begin(DEMO_NS, true);
  String saved = prefs.getString(DEMO_KEY, "timer");
  prefs.end();
  strncpy(activeDemo, saved.c_str(), 15);
  activeDemo[15] = 0;

  // --- Soft-AP ---
  WiFi.softAP(SOFT_AP_SSID, SOFT_AP_PSK);

  // --- Web routes ---
  server.on("/",           handleRoot);
  server.on("/set",        handleSet);
  server.on("/status",     handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();

  // Signal ready on serial
  Serial.println();
  Serial.println("OneBoard Stage 1 ready");
  Serial.print("AP SSID: "); Serial.println(SOFT_AP_SSID);
  Serial.print("AP IP:   "); Serial.println(WiFi.softAPIP());
  Serial.print("Demo:    "); Serial.println(activeDemo);

  // Initial boot screen → handoff to demo
  delay(1500);
  demoChanged = true;
}

/* ==========================================================
   Main loop
   ========================================================== */
void loop() {
  server.handleClient();
  runActiveDemo();
  delay(5);
}