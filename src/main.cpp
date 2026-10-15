/* ==========================================================
   OneBoard Stage 2 — Behavior Store over WiFi (contract-conformant)
   Behavior-definition schema follows the 8-point contract
   (docs/GPT-HANDOFF.md, schema of record):
     - {"slot":"name"} for state refs; literals everywhere else
     - guards = structured comparisons ({"op":...,"a":...,"b":...}), never string-eval
     - named events, ordered handlers, first-match-guard-wins
     - explicit touch regions emitting a named event
     - bounded scheduled events (named timer ids, same-id-replaces)
     - straight-line verb lists; no loops/recursion/calls in data
   ========================================================== */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

/* ==========================================================
   XPT2046 touch — SEPARATE SPI instance (VSPI), independent of the TFT (HSPI).
   This board (2432S028R / E32R28T): SCLK25 MOSI32 MISO39 CS33 IRQ36.
   Uses the stock XPT2046_Touchscreen library on its own VSPI bus — the SAME
   proven profile the working firmwares (Bruce / HaleHound / Wireless Wizard)
   use. Requires USE_HSPI_PORT in build flags so the display owns HSPI and the
   touch owns VSPI (without it both fight for VSPI and touch never reads).
   ========================================================== */
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(33, 36);   // CS, IRQ — same as working profile
#define TOUCH_SCLK 25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39

void touchBegin() {
  touchSPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, 33);
  touch.begin(touchSPI);
  touch.setRotation(0);   /* portrait 240x320 — matches the tool definitions */
}

/* Returns true + screen coords (portrait 240x320) on press. Stock lib does the
   heavy lifting (Z1/Z2 pressure, oversampling, power-down); we map its raw
   rotation-0 output (0..4095) to the screen. */
static bool touchReadScreen(uint16_t *sx, uint16_t *sy) {
  if (!(touch.tirqTouched() && touch.touched())) return false;
  TS_Point p = touch.getPoint();
  if (p.z < 300) return false;
  /* rotation 0: xraw=4095-y, yraw=x (raw 0..4095) → screen 240 wide / 320 tall */
  uint32_t rx = (uint32_t)p.x * 240 / 4096;
  uint32_t ry = (uint32_t)p.y * 320 / 4096;
  if (rx >= 240) rx = 239;
  if (ry >= 320) ry = 319;
  *sx = (uint16_t)rx;
  *sy = (uint16_t)ry;
  return true;
}

/* ==========================================================
   Configuration
   ========================================================== */
#define TFT_BL          21
#define SOFT_AP_SSID    "OneBoard-Stage2"
#define SOFT_AP_PSK     ""

/* NVS namespaces/keys */
#define CFG_NS          "obm_cfg"
#define WIFI_SSID_KEY   "w_ssid"
#define WIFI_PASS_KEY   "w_pass"
#define STORE_KEY       "store_url"
#define ACTIVE_KEY      "active_tool"

/* Defaults */
#define DEFAULT_STORE   "http://192.168.4.100:8080"
#define DEFAULT_TOOL    "timer"

/* Timing */
#define LOOP_DELAY      5

#define BEHAVIOR_DIR    "/behaviors/"
#define JSON_CAPACITY   8192
#define MAX_EVENTS      8

/* ==========================================================
   Globals
   ========================================================== */
TFT_eSPI      tft = TFT_eSPI();
WebServer     server(80);
Preferences   prefs;
bool          wifiStationOk  = false;

String        activeToolId   = DEFAULT_TOOL;
bool          behaviorLoaded = false;
JsonDocument  behaviorDoc;
JsonObject    behaviorRoot;
JsonDocument  stateDoc;
JsonObject    stateRoot;

String storeUrl = DEFAULT_STORE;

/* Per-event last-fire timers (bounded named timers; same-id replaces on reload) */
struct EventTimer { const char* name; unsigned long last; };
EventTimer evTimers[MAX_EVENTS];
int        evTimerCount = 0;

/* ==========================================================
   NVS config helpers
   ========================================================== */
String loadConfigStr(const char* key, const char* fallback) {
  prefs.begin(CFG_NS, true);
  String val = prefs.getString(key, fallback);
  prefs.end();
  return val;
}

void saveConfigStr(const char* key, const char* value) {
  prefs.begin(CFG_NS, false);
  prefs.putString(key, value);
  prefs.end();
}

/* ==========================================================
   Colour mapping
   ========================================================== */
static uint16_t resolveColour(const char* name) {
  if (!name || !name[0]) return TFT_WHITE;
  if      (strcmp(name, "black")     == 0) return TFT_BLACK;
  else if (strcmp(name, "white")     == 0) return TFT_WHITE;
  else if (strcmp(name, "red")       == 0) return TFT_RED;
  else if (strcmp(name, "green")     == 0) return TFT_GREEN;
  else if (strcmp(name, "blue")      == 0) return TFT_BLUE;
  else if (strcmp(name, "yellow")    == 0) return TFT_YELLOW;
  else if (strcmp(name, "cyan")      == 0) return TFT_CYAN;
  else if (strcmp(name, "magenta")   == 0) return TFT_MAGENTA;
  else if (strcmp(name, "orange")    == 0) return TFT_ORANGE;
  else if (strcmp(name, "navy")      == 0) return TFT_NAVY;
  else if (strcmp(name, "darkgrey")  == 0) return TFT_DARKGREY;
  else if (strcmp(name, "darkgray")  == 0) return TFT_DARKGREY;
  else if (strcmp(name, "grey")      == 0) return TFT_DARKGREY;
  else if (strcmp(name, "gray")      == 0) return TFT_DARKGREY;
  return TFT_WHITE;
}

/* ==========================================================
   Value resolution — {"slot":"name"} | {"lit":X} | {"noise":{lo,hi}} | literal
   ========================================================== */
static JsonVariant resolveVar(JsonVariant v) {
  if (v.is<JsonObject>()) {
    if (v["slot"].is<const char*>()) return stateRoot[v["slot"].as<const char*>()];
    if (v.containsKey("lit"))        return v["lit"];
  }
  return v;
}

static float varFloat(JsonVariant v); // forward decl (varInt uses it for noise)

static int64_t varInt(JsonVariant v) {
  if (v.is<JsonObject>()) {
    if (v["slot"].is<const char*>()) return (int64_t)(stateRoot[v["slot"].as<const char*>()] | (int64_t)0);
    if (v.containsKey("lit"))        return varInt(v["lit"]);
    if (v.containsKey("noise")) {
      JsonObject n = v["noise"];
      int lo = (int)varFloat(n["lo"]), hi = (int)varFloat(n["hi"]);
      return (int64_t)random(lo, hi);
    }
  }
  if (v.isNull()) return 0;
  return (int64_t)(v | (int64_t)0);
}

static float varFloat(JsonVariant v) {
  if (v.is<JsonObject>()) {
    if (v["slot"].is<const char*>()) return stateRoot[v["slot"].as<const char*>()] | 0.0f;
    if (v.containsKey("lit"))        return varFloat(v["lit"]);
    if (v.containsKey("noise")) {
      JsonObject n = v["noise"];
      float lo = n["lo"] | -2.0f, hi = n["hi"] | 2.0f;
      return ((float)random(0, 100) / 100.0f) * (hi - lo) + lo;
    }
  }
  if (v.isNull()) return 0.0f;
  return v | 0.0f;
}

static bool varBool(JsonVariant v) {
  if (v.is<JsonObject>()) {
    if (v["slot"].is<const char*>()) return (bool)(stateRoot[v["slot"].as<const char*>()] | false);
    if (v.containsKey("lit"))        return varBool(v["lit"]);
  }
  if (v.isNull()) return false;
  return (bool)(v | false);
}

static String varStr(JsonVariant v) {
  if (v.is<JsonObject>()) {
    if (v["slot"].is<const char*>()) {
      JsonVariant slot = stateRoot[v["slot"].as<const char*>()];
      if (slot.is<const char*>()) return String((const char*)slot);
      if (slot.is<float>())       { char b[16]; snprintf(b, sizeof(b), "%.1f", (float)slot); return String(b); }
      return String((int64_t)(slot | (int64_t)0));
    }
    if (v.containsKey("lit")) return varStr(v["lit"]);
  }
  if (v.is<const char*>()) return String((const char*)v);
  if (v.is<float>())       { char b[16]; snprintf(b, sizeof(b), "%.1f", (float)v); return String(b); }
  if (v.isNull()) return "";
  return String((int64_t)(v | (int64_t)0));
}

static bool varEq(JsonVariant a, JsonVariant b) {
  a = resolveVar(a); b = resolveVar(b);
  if (a.is<const char*>() || b.is<const char*>()) return varStr(a) == varStr(b);
  if (a.is<bool>() || b.is<bool>()) return varBool(a) == varBool(b);   /* bools compare as bools — float cast mangles them */
  return varFloat(a) == varFloat(b);
}

/* ==========================================================
   State management
   ========================================================== */
void initStateFromDef() {
  JsonArray defs = behaviorRoot["state"];
  stateDoc.clear();
  stateRoot = stateDoc.to<JsonObject>();
  if (!defs) return;
  for (JsonObject slot : defs) {
    const char* n  = slot["name"];
    const char* ty = slot["type"];
    if (!n || !ty) continue;
    if      (strcmp(ty, "int64")  == 0) stateRoot[n] = (int64_t)(slot["init"] | 0);
    else if (strcmp(ty, "bool")   == 0) stateRoot[n] = (bool)(slot["init"] | false);
    else if (strcmp(ty, "float")  == 0) stateRoot[n] = slot["init"].is<float>() ? (float)slot["init"] : 0.0f;
    else if (strcmp(ty, "string") == 0) stateRoot[n] = slot["init"] | "";
    else                               stateRoot[n] = (int64_t)(slot["init"] | 0);
  }
}

void resetEventTimers() {
  evTimerCount = 0;
  if (!behaviorRoot["events"]) return;
  for (JsonObject ev : behaviorRoot["events"].as<JsonArray>()) {
    const char* nm = ev["name"];
    if (nm && evTimerCount < MAX_EVENTS) {
      evTimers[evTimerCount].name = strdup(nm);
      evTimers[evTimerCount].last = 0;
      evTimerCount++;
    }
  }
}

static unsigned long eventLast(const char* name) {
  for (int i = 0; i < evTimerCount; i++) if (strcmp(evTimers[i].name, name) == 0) return evTimers[i].last;
  return 0;
}
static void eventSetLast(const char* name, unsigned long t) {
  for (int i = 0; i < evTimerCount; i++) if (strcmp(evTimers[i].name, name) == 0) { evTimers[i].last = t; return; }
}

/* ==========================================================
   Guard evaluation — structured comparisons only
   ========================================================== */
static bool evalGuard(JsonVariant g) {
  if (g.isNull()) return true;
  JsonObject o = g.as<JsonObject>();
  if (!o) return true;
  const char* op = o["op"];
  if (!op || strcmp(op, "always") == 0) return true;
  JsonVariant a = o["a"], b = o["b"];
  if (strcmp(op, "eq") == 0) return varEq(a, b);
  else if (strcmp(op, "ne") == 0) return !varEq(a, b);
  else if (strcmp(op, "lt") == 0) return varFloat(a) <  varFloat(b);
  else if (strcmp(op, "le") == 0) return varFloat(a) <= varFloat(b);
  else if (strcmp(op, "gt") == 0) return varFloat(a) >  varFloat(b);
  else if (strcmp(op, "ge") == 0) return varFloat(a) >= varFloat(b);
  else if (strcmp(op, "is_true") == 0) return varBool(a);
  else if (strcmp(op, "elapsed_ge") == 0) {
    int64_t since = varInt(a);
    int64_t ms    = varInt(b);
    return since > 0 && (millis() - (unsigned long)since) >= (unsigned long)ms;
  }
  return false;
}

/* ==========================================================
   Render primitives (declarative; params are literal-or-slot values)
   ========================================================== */
void renderPrimitive(const char* type, JsonObject p) {
  if (!type) return;
  if (strcmp(type, "clear") == 0) { tft.fillScreen(resolveColour(p["color"])); return; }
  if (strcmp(type, "text") == 0) {
    int32_t x = (int32_t)varInt(p["x"]), y = (int32_t)varInt(p["y"]), f = (int32_t)varInt(p["font"]) | 2;
    if (f <= 0) f = 2;
    tft.setTextColor(resolveColour(p["color"]), TFT_BLACK);
    tft.drawString(varStr(p["src"]).c_str(), x, y, f);
    return;
  }
  if (strcmp(type, "rect") == 0) {
    tft.drawRect((int32_t)varInt(p["x"]), (int32_t)varInt(p["y"]),
                 (int32_t)varInt(p["w"]), (int32_t)varInt(p["h"]),
                 resolveColour(p["color"]));
    return;
  }
  if (strcmp(type, "fillrect") == 0) {
    tft.fillRect((int32_t)varInt(p["x"]), (int32_t)varInt(p["y"]),
                 (int32_t)varInt(p["w"]), (int32_t)varInt(p["h"]),
                 resolveColour(p["color"]));
    return;
  }
  if (strcmp(type, "line") == 0) {
    tft.drawLine((int32_t)varInt(p["x0"]), (int32_t)varInt(p["y0"]),
                 (int32_t)varInt(p["x1"]), (int32_t)varInt(p["y1"]),
                 resolveColour(p["color"]));
    return;
  }
  if (strcmp(type, "circle") == 0) {
    tft.fillCircle((int32_t)varInt(p["x"]), (int32_t)varInt(p["y"]),
                   (int32_t)varInt(p["r"]), resolveColour(p["color"]));
    return;
  }
  if (strcmp(type, "bar") == 0) {
    int32_t x = (int32_t)varInt(p["x"]), y = (int32_t)varInt(p["y"]);
    int32_t w = (int32_t)varInt(p["w"]), h = (int32_t)varInt(p["h"]);
    uint16_t c = resolveColour(p["color"]);
    if (w > 0) tft.fillRect(x, y, w, h, c);
    tft.drawRect(x, y, w, h, c);
    return;
  }
}

/* graph — temperature rolling line graph (temp-logger) */
void renderGraph(JsonObject p) {
#define GRAPH_SIZE 100
  static float hist[GRAPH_SIZE];
  static int   hCnt = 0;
  float currentTemp = varFloat(p["slot"]);
  int64_t sIdx = stateRoot["sample_idx"] | (int64_t)0;
  if (hCnt < GRAPH_SIZE) hCnt++;
  hist[sIdx % GRAPH_SIZE] = currentTemp;
  int graphTop = 100, graphBot = 280;
  tft.drawRect(10, graphTop, 220, graphBot - graphTop, TFT_DARKGREY);
  int n = hCnt;
  if (n > 1) {
    int drawW = 220 / (n - 1);
    if (drawW < 1) drawW = 1;
    if (drawW > 4) drawW = 4;
    for (int i = 0; i < n - 1; i++) {
      int idx0 = (sIdx + i) % GRAPH_SIZE;
      int idx1 = (sIdx + i + 1) % GRAPH_SIZE;
      float v0 = hist[idx0], v1 = hist[idx1];
      int y0 = graphBot - ((v0 - 60.0f) / 30.0f) * (graphBot - graphTop);
      int y1 = graphBot - ((v1 - 60.0f) / 30.0f) * (graphBot - graphTop);
      int x0 = 10 + i * drawW, x1 = 10 + (i + 1) * drawW;
      if (x0 >= 10 && x1 <= 230) tft.drawLine(x0, y0, x1, y1, TFT_GREEN);
    }
  }
}

/* ==========================================================
   Verb execution
   ========================================================== */
void execBlock(JsonArray block);

/* True if the named state slot is declared as type "float" in the behavior def. */
static bool slotIsFloat(const char* name) {
  JsonArray defs = behaviorRoot["state"];
  if (!defs) return false;
  for (JsonObject s : defs) {
    if (s["name"] && strcmp(s["name"], name) == 0) {
      const char* ty = s["type"] | "int64";
      return (strcmp(ty, "float") == 0);
    }
  }
  return false;
}

void execVerb(JsonObject verb) {
  if (!verb) return;

  /* add: {"add":{"slot":"x","delta":<value>}} */
  if (verb.containsKey("add")) {
    JsonObject p = verb["add"];
    const char* name = p["slot"];
    if (name) {
      float delta = varFloat(p["delta"]);
      if (slotIsFloat(name)) {
        float nv = (stateRoot[name] | 0.0f) + delta;
        stateRoot[name] = nv;
      } else {
        int64_t nv = (int64_t)(stateRoot[name] | (int64_t)0) + (int64_t)delta;
        stateRoot[name] = nv;
      }
    }
    return;
  }

  /* sub: {"sub":{"slot":"x","delta":<value>}} */
  if (verb.containsKey("sub")) {
    JsonObject p = verb["sub"];
    const char* name = p["slot"];
    if (name) {
      int64_t nv = (int64_t)(stateRoot[name] | (int64_t)0) - (int64_t)varInt(p["delta"]);
      stateRoot[name] = nv;
    }
    return;
  }

  /* set: {"set":{"slot":"x","value":<literal>}} */
  if (verb.containsKey("set")) {
    JsonObject p = verb["set"];
    const char* name = p["slot"];
    if (name) stateRoot[name].set(resolveVar(p["value"]));
    return;
  }

  /* setf: {"setf":{"slot":"x","value":<number>}} */
  if (verb.containsKey("setf")) {
    JsonObject p = verb["setf"];
    const char* name = p["slot"];
    if (name) stateRoot[name] = varFloat(p["value"]);
    return;
  }

  /* toggle: {"toggle":{"slot":"x"}} */
  if (verb.containsKey("toggle")) {
    JsonObject p = verb["toggle"];
    const char* name = p["slot"];
    if (name) {
      bool val = (bool)(stateRoot[name] | false);
      stateRoot[name] = !val;
    }
    return;
  }

  /* stamp: {"stamp":{"slot":"x"}} — record millis() */
  if (verb.containsKey("stamp")) {
    JsonObject p = verb["stamp"];
    const char* name = p["slot"];
    if (name) stateRoot[name] = (int64_t)millis();
    return;
  }

  /* now_minus: {"now_minus":{"slot":"x","from":<value>}} — x = millis() - from */
  if (verb.containsKey("now_minus")) {
    JsonObject p = verb["now_minus"];
    const char* name = p["slot"];
    if (name) stateRoot[name] = (int64_t)(millis() - (unsigned long)varInt(p["from"]));
    return;
  }

  /* format: {"format":{"slot":"x","op":"ms"|"temp","src":<value>}} */
  if (verb.containsKey("format")) {
    JsonObject p = verb["format"];
    const char* name = p["slot"];
    const char* op = p["op"];
    if (name && op) {
      if (strcmp(op, "ms") == 0) {
        int64_t ms = varInt(p["src"]);
        char b[12]; snprintf(b, sizeof(b), "%02lld:%02lld", ms / 60000, (ms % 60000) / 1000);
        stateRoot[name] = b;
      } else if (strcmp(op, "temp") == 0) {
        char b[16]; snprintf(b, sizeof(b), "%.1f F", varFloat(p["src"]));
        stateRoot[name] = b;
      }
    }
    return;
  }

  /* progress: {"progress":{"slot":"x","src":<value>,"max":<value>,"px":<value>}} */
  if (verb.containsKey("progress")) {
    JsonObject p = verb["progress"];
    const char* name = p["slot"];
    if (name) {
      int64_t max = varInt(p["max"]); if (max <= 0) max = 1;
      int64_t w = (varInt(p["src"]) % max) * varInt(p["px"]) / max;
      stateRoot[name] = (int64_t)w;
    }
    return;
  }

  /* render: {"render":{"type":"text","x":...,"src":...}} */
  if (verb.containsKey("render")) {
    JsonObject p = verb["render"];
    const char* t = p["type"];
    if (t && strcmp(t, "graph") == 0) { renderGraph(p); return; }
    if (t) renderPrimitive(t, p);
    return;
  }

  /* if: {"if":{"guard":{...},"then":[...],"else":[...]}} */
  if (verb.containsKey("if")) {
    JsonObject p = verb["if"];
    if (evalGuard(p["guard"])) { if (p["then"].is<JsonArray>()) execBlock(p["then"]); }
    else                       { if (p["else"].is<JsonArray>()) execBlock(p["else"]); }
    return;
  }
}

void execBlock(JsonArray block) {
  if (!block) return;
  for (JsonObject sv : block) execVerb(sv);
}

/* Dispatch a named event: first matching handler's guard wins, then stop. */
void dispatchEvent(const char* eventName) {
  if (!behaviorLoaded || !behaviorRoot["events"]) return;
  for (JsonObject ev : behaviorRoot["events"].as<JsonArray>()) {
    const char* nm = ev["name"];
    if (!nm || strcmp(nm, eventName) != 0) continue;
    JsonArray handlers = ev["handlers"];
    if (!handlers) return;
    for (JsonObject h : handlers) {
      if (evalGuard(h["guard"])) { if (h["run"].is<JsonArray>()) execBlock(h["run"]); return; }
    }
    return;
  }
}

/* ==========================================================
   Behavior loading
   ========================================================== */
bool setBehaviorFromJson(const char* jsonStr) {
  DeserializationError err = deserializeJson(behaviorDoc, jsonStr);
  if (err) { Serial.print("JSON err: "); Serial.println(err.c_str()); return false; }
  behaviorRoot = behaviorDoc.as<JsonObject>();
  if (!behaviorRoot["id"]) { Serial.println("No 'id'"); return false; }
  initStateFromDef();
  resetEventTimers();
  behaviorLoaded = true;
  const char* id = behaviorRoot["id"];
  if (id) { activeToolId = String(id); saveConfigStr(ACTIVE_KEY, id); }
  return true;
}

bool fetchBehavior(const char* toolId) {
  if (!wifiStationOk) return false;
  HTTPClient http;
  String url = storeUrl;
  if (!url.endsWith("/")) url += "/";
  url += String(toolId) + ".json";
  Serial.print("Fetch: "); Serial.println(url);
  http.begin(url.c_str()); http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString(); http.end();
    File f = LittleFS.open(String(BEHAVIOR_DIR) + String(toolId) + ".json", "w");
    if (f) { f.print(payload); f.close(); }
    return setBehaviorFromJson(payload.c_str());
  }
  Serial.print("HTTP: "); Serial.println(code); http.end();
  return false;
}

bool loadCachedBehavior(const char* toolId) {
  String path = String(BEHAVIOR_DIR) + String(toolId) + ".json";
  if (!LittleFS.exists(path.c_str())) return false;
  File f = LittleFS.open(path.c_str(), "r");
  if (!f) return false;
  String jsonStr;
  while (f.available()) jsonStr += (char)f.read();
  f.close();
  return setBehaviorFromJson(jsonStr.c_str());
}

/* ==========================================================
   — contract-conformant: {"slot":".."} refs, structured guards,
     named events / first-match-guard-wins, touch regions
   ========================================================== */
const char TIMER_JSON[] PROGMEM =
  "{\"id\":\"timer\",\"name\":\"Count-Up Timer\",\"layout\":\"fullscreen\","
  "\"state\":[{\"name\":\"elapsed_ms\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"running\",\"type\":\"bool\",\"init\":true},"
  "{\"name\":\"elapsed_txt\",\"type\":\"string\",\"init\":\"00:00\"},"
  "{\"name\":\"progress_w\",\"type\":\"int64\",\"init\":0}],"
  "\"events\":["
  "{\"name\":\"tick\",\"schedule\":{\"type\":\"interval\",\"ms\":1000},\"handlers\":["
  "{\"guard\":{\"op\":\"eq\",\"a\":{\"slot\":\"running\"},\"b\":{\"lit\":true}},\"run\":["
  "{\"add\":{\"slot\":\"elapsed_ms\",\"delta\":{\"lit\":1000}}},"
  "{\"format\":{\"slot\":\"elapsed_txt\",\"op\":\"ms\",\"src\":{\"slot\":\"elapsed_ms\"}}},"
  "{\"progress\":{\"slot\":\"progress_w\",\"src\":{\"slot\":\"elapsed_ms\"},\"max\":{\"lit\":3600000},\"px\":{\"lit\":220}}},"
  "{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":20},\"y\":{\"lit\":60},\"src\":{\"lit\":\"Timer\"},\"font\":{\"lit\":2},\"color\":\"yellow\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":50},\"y\":{\"lit\":110},\"src\":{\"slot\":\"elapsed_txt\"},\"font\":{\"lit\":6},\"color\":\"green\"}},"
  "{\"render\":{\"type\":\"bar\",\"x\":{\"lit\":10},\"y\":{\"lit\":170},\"w\":{\"slot\":\"progress_w\"},\"h\":{\"lit\":10},\"color\":\"blue\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":30},\"y\":{\"lit\":210},\"src\":{\"lit\":\"Touch to pause/resume\"},\"font\":{\"lit\":2},\"color\":\"white\"}}"
  "]}]},"
  "{\"name\":\"touch-down\",\"region\":{\"x\":0,\"y\":0,\"w\":240,\"h\":320},\"handlers\":["
  "{\"run\":[{\"toggle\":{\"slot\":\"running\"}}]}"
  "]}]}";

const char TEMP_JSON[] PROGMEM =
  "{\"id\":\"temp-logger\",\"name\":\"Temp Logger\",\"layout\":\"fullscreen\","
  "\"state\":[{\"name\":\"temp\",\"type\":\"float\",\"init\":72.0},"
  "{\"name\":\"last_tick\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"sample_idx\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"sample_cnt\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"temp_txt\",\"type\":\"string\",\"init\":\"72.0 F\"}],"
  "\"events\":["
  "{\"name\":\"tick\",\"schedule\":{\"type\":\"interval\",\"ms\":250},\"handlers\":["
  "{\"guard\":{\"op\":\"always\"},\"run\":["
  "{\"stamp\":{\"slot\":\"last_tick\"}},"
  "{\"add\":{\"slot\":\"temp\",\"delta\":{\"noise\":{\"lo\":-2.0,\"hi\":2.0}}}},"
  "{\"if\":{\"guard\":{\"op\":\"lt\",\"a\":{\"slot\":\"temp\"},\"b\":{\"lit\":60}},\"then\":[{\"setf\":{\"slot\":\"temp\",\"value\":{\"lit\":60}}}]}},"
  "{\"if\":{\"guard\":{\"op\":\"gt\",\"a\":{\"slot\":\"temp\"},\"b\":{\"lit\":90}},\"then\":[{\"setf\":{\"slot\":\"temp\",\"value\":{\"lit\":90}}}]}},"
  "{\"format\":{\"slot\":\"temp_txt\",\"op\":\"temp\",\"src\":{\"slot\":\"temp\"}}},"
  "{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":80},\"y\":{\"lit\":40},\"src\":{\"lit\":\"Temp Logger\"},\"font\":{\"lit\":2},\"color\":\"yellow\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":70},\"y\":{\"lit\":70},\"src\":{\"slot\":\"temp_txt\"},\"font\":{\"lit\":4},\"color\":\"cyan\"}},"
  "{\"render\":{\"type\":\"rect\",\"x\":{\"lit\":10},\"y\":{\"lit\":100},\"w\":{\"lit\":220},\"h\":{\"lit\":180},\"color\":\"darkgrey\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":215},\"y\":{\"lit\":100},\"src\":{\"lit\":\"90F\"},\"font\":{\"lit\":1},\"color\":\"white\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":215},\"y\":{\"lit\":270},\"src\":{\"lit\":\"60F\"},\"font\":{\"lit\":1},\"color\":\"white\"}},"
  "{\"render\":{\"type\":\"graph\",\"slot\":{\"slot\":\"temp\"}}},"
  "{\"add\":{\"slot\":\"sample_idx\",\"delta\":{\"lit\":1}}},"
  "{\"if\":{\"guard\":{\"op\":\"lt\",\"a\":{\"slot\":\"sample_cnt\"},\"b\":{\"lit\":100}},\"then\":[{\"add\":{\"slot\":\"sample_cnt\",\"delta\":{\"lit\":1}}}]}}"
  "]}]]}";

const char REACTION_JSON[] PROGMEM =
  "{\"id\":\"reaction-game\",\"name\":\"Reaction Game\",\"layout\":\"fullscreen\","
  "\"state\":[{\"name\":\"phase\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"wait_start\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"go_at\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"delay_ms\",\"type\":\"int64\",\"init\":3000},"
  "{\"name\":\"touch_ms\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"show_until\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"result_txt\",\"type\":\"string\",\"init\":\"00:00\"}],"
  "\"events\":["
  "{\"name\":\"tick\",\"schedule\":{\"type\":\"interval\",\"ms\":100},\"handlers\":["
  "{\"guard\":{\"op\":\"eq\",\"a\":{\"slot\":\"phase\"},\"b\":{\"lit\":0}},\"run\":["
  "{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":70},\"y\":{\"lit\":40},\"src\":{\"lit\":\"Reaction Game\"},\"font\":{\"lit\":2},\"color\":\"yellow\"}},"
  "{\"render\":{\"type\":\"circle\",\"x\":{\"lit\":120},\"y\":{\"lit\":140},\"r\":{\"lit\":50},\"color\":\"green\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":65},\"y\":{\"lit\":200},\"src\":{\"lit\":\"READY...\"},\"font\":{\"lit\":2},\"color\":\"white\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":30},\"y\":{\"lit\":240},\"src\":{\"lit\":\"(wait for GO)\"},\"font\":{\"lit\":2},\"color\":\"darkgrey\"}},"
  "{\"stamp\":{\"slot\":\"wait_start\"}},"
  "{\"set\":{\"slot\":\"phase\",\"value\":{\"lit\":1}}}"
  "]},"
  "{\"guard\":{\"op\":\"eq\",\"a\":{\"slot\":\"phase\"},\"b\":{\"lit\":1}},\"run\":["
  "{\"if\":{\"guard\":{\"op\":\"elapsed_ge\",\"a\":{\"slot\":\"wait_start\"},\"b\":{\"slot\":\"delay_ms\"}},\"then\":["
  "{\"set\":{\"slot\":\"phase\",\"value\":{\"lit\":2}}},"
  "{\"stamp\":{\"slot\":\"go_at\"}},"
  "{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":70},\"y\":{\"lit\":40},\"src\":{\"lit\":\"Reaction Game\"},\"font\":{\"lit\":2},\"color\":\"yellow\"}},"
  "{\"render\":{\"type\":\"circle\",\"x\":{\"lit\":120},\"y\":{\"lit\":140},\"r\":{\"lit\":50},\"color\":\"red\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":75},\"y\":{\"lit\":210},\"src\":{\"lit\":\"TAP!\"},\"font\":{\"lit\":2},\"color\":\"white\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":30},\"y\":{\"lit\":240},\"src\":{\"lit\":\"Touch the screen!\"},\"font\":{\"lit\":2},\"color\":\"white\"}}"
  "]}]},"
  "{\"guard\":{\"op\":\"eq\",\"a\":{\"slot\":\"phase\"},\"b\":{\"lit\":2}},\"run\":["
  "{\"if\":{\"guard\":{\"op\":\"elapsed_ge\",\"a\":{\"slot\":\"show_until\"},\"b\":{\"lit\":3000}},\"then\":["
  "{\"set\":{\"slot\":\"phase\",\"value\":{\"lit\":0}}}]}]}"
  "]},"
  "{\"name\":\"touch-down\",\"region\":{\"x\":0,\"y\":0,\"w\":240,\"h\":320},\"handlers\":["
  "{\"guard\":{\"op\":\"eq\",\"a\":{\"slot\":\"phase\"},\"b\":{\"lit\":2}},\"run\":["
  "{\"now_minus\":{\"slot\":\"touch_ms\",\"from\":{\"slot\":\"go_at\"}}},"
  "{\"format\":{\"slot\":\"result_txt\",\"op\":\"ms\",\"src\":{\"slot\":\"touch_ms\"}}},"
  "{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":70},\"y\":{\"lit\":40},\"src\":{\"lit\":\"Reaction Game\"},\"font\":{\"lit\":2},\"color\":\"yellow\"}},"
  "{\"render\":{\"type\":\"text\",\"x\":{\"lit\":40},\"y\":{\"lit\":110},\"src\":{\"slot\":\"result_txt\"},\"font\":{\"lit\":6},\"color\":\"green\"}},"
  "{\"stamp\":{\"slot\":\"show_until\"}}"
  "]}]}"
  "]}]}";

/* ==========================================================
   Behavior loading (needs the PROGMEM arrays above for the built-in fallback)
   ========================================================== */
const char* progmemBehavior(const char* toolId) {
  if (!toolId) return NULL;
  if (strcmp(toolId, "timer") == 0)         return TIMER_JSON;
  if (strcmp(toolId, "temp-logger") == 0)   return TEMP_JSON;
  if (strcmp(toolId, "reaction-game") == 0) return REACTION_JSON;
  return NULL;
}

void loadBehavior(const char* toolId) {
  Serial.print("Load: "); Serial.println(toolId);
  if (wifiStationOk && fetchBehavior(toolId)) { Serial.println("OK store"); return; }
  if (loadCachedBehavior(toolId)) { Serial.println("OK cache"); return; }
  /* Fall back to the compiled-in PROGMEM copy — no filesystem needed. Guarantees the board
     always has behavior (never blank), even if LittleFS is empty/unformatted. */
  const char* builtin = progmemBehavior(toolId);
  if (builtin && setBehaviorFromJson(builtin)) { Serial.println("OK builtin (PROGMEM)"); return; }
  Serial.println("No behavior"); behaviorLoaded = false;
}

/* Seed builtins to LittleFS on first boot */
void seedDefaultBehaviors() {
  const char* ids[3] = {"timer", "temp-logger", "reaction-game"};
  const char* js[3]  = {TIMER_JSON, TEMP_JSON, REACTION_JSON};
  for (int i = 0; i < 3; i++) {
    String path = String(BEHAVIOR_DIR) + String(ids[i]) + ".json";
    if (!LittleFS.exists(path.c_str())) {
      File f = LittleFS.open(path.c_str(), "w");
      if (f) { f.print(js[i]); f.close(); Serial.print("Seed: "); Serial.println(path); }
    }
  }
}

/* ==========================================================
   WiFi helpers
   ========================================================== */
void tryStationConnect() {
  String ssid = loadConfigStr(WIFI_SSID_KEY, "");
  String pass = loadConfigStr(WIFI_PASS_KEY, "");
  if (ssid.length() == 0) return;
  Serial.print("WiFi: "); Serial.println(ssid);
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) { delay(500); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifiStationOk = true;
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    wifiStationOk = false;
    Serial.println("WiFi fail — AP fallback");
  }
}

/* ==========================================================
   Web server handlers
   ========================================================== */
void handleRoot() {
  String html = F("<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=320'>"
    "<title>OneBoard Stage 2</title>"
    "<style>"
      "*{box-sizing:border-box;margin:0;padding:0}"
      "body{font-family:-apple-system,Helvetica,sans-serif;background:#111;color:#ddd;text-align:center;padding:20px}"
      "h1{font-size:1.3em;margin-bottom:10px;color:#4af}"
      ".store-url{font-size:0.8em;color:#888;margin-bottom:6px}"
      ".demo-name{font-size:1.1em;margin:12px 0;color:#ff0}"
      "button{display:block;width:100%;max-width:280px;margin:10px auto;padding:14px;"
        "border:2px solid #555;border-radius:10px;background:#222;color:#fff;font-size:1.1em;cursor:pointer}"
      "button:hover{border-color:#4af;background:#333}"
      ".active{background:#4af;color:#000;border-color:#4af}"
      ".footer{margin-top:18px;font-size:0.8em;color:#666}"
    "</style></head><body>"
    "<h1>&#9670; OneBoard Stage 2</h1>"
    "<div class='store-url' id='storeLabel'></div>"
    "<div class='demo-name' id='current'></div>"
    "<button id='btn-timer' onclick='loadTool(\"timer\")'>&#9201; Timer</button>"
    "<button id='btn-temp-logger' onclick='loadTool(\"temp-logger\")'>&#127777; Temp Logger</button>"
    "<button id='btn-reaction-game' onclick='loadTool(\"reaction-game\")'>&#9889; Reaction Game</button>"
    "<div style='margin-top:18px;border-top:1px solid #444;padding-top:12px'>"
      "<input id='storeInput' type='text' style='width:100%;max-width:280px;padding:8px;background:#222;color:#fff;border:1px solid #555'"
        " placeholder='Store URL' />"
      "<button onclick='setStore()' style='background:#333;border-color:#4af'>Set Store URL</button>"
    "</div>"
    "<div class='footer'>OneBoard Stage 2 — CYD 2432S028</div>"
    "<script>"
      "async function loadTool(id){"
        "try{"
          "var r=await fetch('/load?id='+id);"
          "var j=await r.json();"
          "if(j.ok)document.getElementById('current').innerText='Tool: '+j.tool"
        "}catch(e){}"
      "}"
      "async function setStore(){"
        "var v=document.getElementById('storeInput').value.trim();"
        "if(!v)return;"
        "try{"
          "var r=await fetch('/store?url='+encodeURIComponent(v));"
          "var j=await r.json();"
          "if(j.ok)document.getElementById('storeLabel').innerText='Store: '+j.url"
        "}catch(e){alert('Failed')}"
      "}"
      "fetch('/status').then(r=>r.json()).then(j=>{"
        "document.getElementById('current').innerText='Tool: '+j.tool;"
        "document.getElementById('storeLabel').innerText='Store: '+j.store_url;"
        "document.getElementById('storeInput').value=j.store_url;"
        "var b=document.getElementById('btn-'+j.tool);"
        "if(b)b.className='active'"
      "}).catch(()=>{})"
    "</script></body></html>");
  server.send(200, "text/html", html);
}

void handleLoad() {
  if (server.hasArg("id")) {
    String id = server.arg("id");
    loadBehavior(id.c_str());
    String json = "{\"ok\":true,\"tool\":\"" + id + "\"}";
    server.send(200, "application/json", json);
    return;
  }
  server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}");
}

void handleStatus() {
  String json = "{\"tool\":\"";
  json += activeToolId;
  json += "\",\"store_url\":\"";
  json += storeUrl;
  json += "\",\"loaded\":";
  json += behaviorLoaded ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

void handleStore() {
  if (server.hasArg("url")) {
    storeUrl = server.arg("url");
    saveConfigStr(STORE_KEY, storeUrl.c_str());
    String json = "{\"ok\":true,\"url\":\"" + storeUrl + "\"}";
    server.send(200, "application/json", json);
    return;
  }
  server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
}

void handleNotFound() {
  server.send(404, "text/plain", "404 Not Found\n");
}

/* ==========================================================
   Setup
   ========================================================== */
void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.init();
  tft.setRotation(0);   /* portrait 240x320 — matches the behavior tool definitions */
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Stage 2 booting...", 10, 10, 2);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount fail — formatting");
    LittleFS.format();
    LittleFS.begin();
  }
  LittleFS.mkdir("/behaviors/");
  touchBegin();

  storeUrl = loadConfigStr(STORE_KEY, DEFAULT_STORE);
  activeToolId = loadConfigStr(ACTIVE_KEY, DEFAULT_TOOL);

  tryStationConnect();

  WiFi.softAP(SOFT_AP_SSID, SOFT_AP_PSK);

  seedDefaultBehaviors();
  loadBehavior(activeToolId.c_str());

  server.on("/",        handleRoot);
  server.on("/load",    handleLoad);
  server.on("/status",  handleStatus);
  server.on("/store",   handleStore);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println();
  Serial.println("OneBoard Stage 2 ready (contract schema)");
  Serial.print("AP:  "); Serial.println(SOFT_AP_SSID);
  Serial.print("IP:  "); Serial.println(WiFi.softAPIP());
  if (wifiStationOk) { Serial.print("STA: "); Serial.println(WiFi.localIP()); }
  Serial.print("Store: "); Serial.println(storeUrl);
  Serial.print("Tool:  "); Serial.println(activeToolId);
}

/* ==========================================================
   Main loop
   ========================================================== */
void loop() {
  server.handleClient();

  /* Scheduled events (bounded named timers) */
  unsigned long now = millis();
  if (behaviorLoaded && behaviorRoot["events"]) {
    for (JsonObject ev : behaviorRoot["events"].as<JsonArray>()) {
      JsonObject sched = ev["schedule"];
      if (!sched) continue;
      const char* stype = sched["type"];
      if (stype && strcmp(stype, "interval") == 0) {
        const char* name = ev["name"];
        if (!name) continue;
        unsigned long interval = (unsigned long)(sched["ms"] | 1000L);
        if (now - eventLast(name) >= interval) {
          eventSetLast(name, now);
          dispatchEvent(name);
        }
      }
    }
  }

  /* Touch-down → named event via explicit region (edge-detected: one event per press).
     Debounce: the stock lib's touched() can flicker during a hold (pressure dips below
     threshold), which would re-trigger the edge. A 250ms cooldown per physical press
     keeps it to exactly one toggle. */
  static bool touchWasDown = false;
  static uint32_t lastTouchEventMs = 0;
  uint16_t tx = 0, ty = 0;
  bool touched = touchReadScreen(&tx, &ty);
  uint32_t tNow = millis();
  if (touched && !touchWasDown && (tNow - lastTouchEventMs > 250) &&
      behaviorLoaded && behaviorRoot["events"]) {
    lastTouchEventMs = tNow;
    for (JsonObject ev : behaviorRoot["events"].as<JsonArray>()) {
      JsonObject region = ev["region"];
      if (!region) continue;
      uint16_t rx = 0, ry = 0, rw = 240, rh = 320;
      rx = (uint16_t)varInt(region["x"]); ry = (uint16_t)varInt(region["y"]);
      rw = (uint16_t)varInt(region["w"]); rh = (uint16_t)varInt(region["h"]);
      if (tx >= rx && tx < rx + rw && ty >= ry && ty < ry + rh) {
        dispatchEvent(ev["name"]);
        break; /* first matching region wins */
      }
    }
  }
  touchWasDown = touched;

  delay(LOOP_DELAY);
}