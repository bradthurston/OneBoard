/* ==========================================================
   OneBoard Stage 2 — Behavior Store over WiFi
   Tools as JSON data; verb interpreter dispatches behaviors
   ========================================================== */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

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
#define TICK_MS         1000
#define LOOP_DELAY      5

#define BEHAVIOR_DIR    "/behaviors/"
#define JSON_CAPACITY   8192

/* ==========================================================
   Globals
   ========================================================== */
TFT_eSPI      tft = TFT_eSPI();
WebServer     server(80);
Preferences   prefs;
bool          wifiStationOk  = false;
unsigned long lastTickMs     = 0;

String        activeToolId   = DEFAULT_TOOL;
bool          toolChanged    = true;
bool          behaviorLoaded = false;
JsonDocument  behaviorDoc;
JsonObject    behaviorRoot;
JsonDocument  stateDoc;
JsonObject    stateRoot;

String storeUrl = DEFAULT_STORE;

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
   State management
   ========================================================== */
void initStateFromDef() {
  JsonArray defs = behaviorRoot["state"];
  if (!defs) return;
  stateDoc.clear();
  stateRoot = stateDoc.to<JsonObject>();
  for (JsonObject slot : defs) {
    const char* n  = slot["name"];
    const char* ty = slot["type"];
    if (!n || !ty) continue;
    if      (strcmp(ty, "int64") == 0) stateRoot[n] = (int64_t)(slot["init"] | 0);
    else if (strcmp(ty, "bool")  == 0) stateRoot[n] = (bool)(slot["init"] | false);
    else if (strcmp(ty, "float") == 0) stateRoot[n] = slot["init"].is<float>() ? (float)slot["init"] : 0.0f;
    else                               stateRoot[n] = (int64_t)(slot["init"] | 0);
  }
}

/* ==========================================================
   Expression evaluators
   ========================================================== */
static String stripState(const char* key) {
  String s(key); s.trim();
  if (s.startsWith("state.")) s = s.substring(6);
  s.trim(); return s;
}

static int64_t stateInt(const char* key) {
  String k = stripState(key);
  if (!k.length()) return 0;
  return stateRoot[k.c_str()] | (int64_t)0;
}

static float stateFloat(const char* key) {
  String k = stripState(key);
  if (!k.length()) return 0.0f;
  float v = stateRoot[k.c_str()];
  return v;
}

static bool stateBool(const char* key) {
  String k = stripState(key);
  if (!k.length()) return false;
  return (bool)(stateRoot[k.c_str()] | false);
}

static void stateSet(const char* key, int64_t val) {
  stateRoot[stripState(key).c_str()] = val;
}

static void stateSetF(const char* key, float val) {
  stateRoot[stripState(key).c_str()] = val;
}

/* Find first '(' in string, or -1 */
static int findParen(const char* s) {
  for (int i = 0; s[i]; i++) if (s[i] == '(') return i;
  return -1;
}

int64_t evalInt(const char* expr) {
  if (!expr || !expr[0]) return 0;
  int p = findParen(expr);
  if (p > 0 && expr[strlen(expr)-1] == ')') {
    String fn = String(expr).substring(0, p);
    String in = String(expr).substring(p+1, strlen(expr)-1); in.trim();
    if (fn == "progress_bar") {
      int64_t el = evalInt(in.c_str());
      return (el % 3600000L) * 220L / 3600000L;
    }
  }
  if (strncmp(expr, "state.", 6) == 0) return stateInt(expr);
  return atol(expr);
}

float evalFloat(const char* expr) {
  if (!expr || !expr[0]) return 0.0f;
  if (strncmp(expr, "state.", 6) == 0) return stateFloat(expr);
  return atof(expr);
}

bool evalBool(const char* expr) {
  if (!expr || !expr[0]) return false;
  int p = findParen(expr);
  if (p > 0 && expr[strlen(expr)-1] == ')') {
    String fn = String(expr).substring(0, p);
    String in = String(expr).substring(p+1, strlen(expr)-1); in.trim();
    if (fn == "is_true")        return evalBool(in.c_str());
    if (fn == "stale_250ms")    return (millis() - (unsigned long)evalInt(in.c_str()) >= 250);
    if (fn == "delay_elapsed") {
      int64_t st = evalInt(in.c_str());
      int64_t d  = stateRoot["delay_ms"] | (int64_t)3000;
      return st > 0 && (millis() - (unsigned long)st >= (unsigned long)d);
    }
    if (fn == "timeout_3s")     return (millis() - (unsigned long)evalInt(in.c_str()) >= 3000);
    if (fn == "clamp_60_90") {
      float v = evalFloat(in.c_str());
      if (v < 60.0f) stateSetF(in.c_str(), 60.0f);
      if (v > 90.0f) stateSetF(in.c_str(), 90.0f);
      return true;
    }
    if (fn == "lt_100")         return evalInt(in.c_str()) < 100;
    return false;
  }
  if (strcmp(expr, "true") == 0) return true;
  if (strncmp(expr, "state.", 6) == 0) return stateBool(expr);
  return atol(expr) != 0;
}

String evalString(const char* expr) {
  if (!expr || !expr[0]) return "";
  int p = findParen(expr);
  if (p > 0 && expr[strlen(expr)-1] == ')') {
    String fn = String(expr).substring(0, p);
    String in = String(expr).substring(p+1, strlen(expr)-1); in.trim();
    if (fn == "format_ms") {
      int64_t ms = evalInt(in.c_str());
      char buf[12];
      snprintf(buf, sizeof(buf), "%02lld:%02lld", ms/60000, (ms%60000)/1000);
      return String(buf);
    }
    if (fn == "format_temp") {
      char buf[16];
      snprintf(buf, sizeof(buf), "%.1f F", evalFloat(in.c_str()));
      return String(buf);
    }
  }
  if (strncmp(expr, "state.", 6) == 0) {
    String key = stripState(expr);
    int64_t iv = stateRoot[key.c_str()] | (int64_t)0;
    float   fv = stateRoot[key.c_str()] | 0.0f;
    if (fv != (float)iv && fv != 0.0f) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%.1f", fv);
      return String(buf);
    }
    return String(iv);
  }
  return String(expr);
}

/* ==========================================================
   Render primitives
   ========================================================== */
void renderPrimitive(const char* type, JsonObject params) {
  if (!type) return;
  if (strcmp(type, "clear") == 0) {
    tft.fillScreen(resolveColour(params["color"])); return;
  }
  if (strcmp(type, "text") == 0) {
    int x = params["x"] | 0, y = params["y"] | 0, f = params["font"] | 2;
    tft.setTextColor(resolveColour(params["color"]), TFT_BLACK);
    tft.drawString(evalString(params["src"]).c_str(), x, y, f);
    return;
  }
  if (strcmp(type, "rect") == 0) {
    tft.drawRect(params["x"]|0, params["y"]|0, params["w"]|0, params["h"]|0,
                 resolveColour(params["color"]));
    return;
  }
  if (strcmp(type, "line") == 0) {
    tft.drawLine(params["x0"]|0, params["y0"]|0, params["x1"]|0, params["y1"]|0,
                 resolveColour(params["color"]));
    return;
  }
  if (strcmp(type, "circle") == 0) {
    tft.fillCircle(params["x"]|0, params["y"]|0, params["r"]|10,
                   resolveColour(params["color"]));
    return;
  }
  if (strcmp(type, "bar") == 0) {
    int x = params["x"]|0, y = params["y"]|0, w = params["w"]|0, h = params["h"]|0;
    uint16_t c = resolveColour(params["color"]);
    tft.fillRect(x, y, w, h, c);
    if (w > 0) tft.drawRect(x, y, w, h, c);
    return;
  }
}

/* ==========================================================
   Verb dispatcher — single verb
   ========================================================== */
void execVerb(JsonObject verb) {
  if (!verb) return;
  const char* v = verb["v"];
  if (!v) return;

  /* lit — literal */
  if (strcmp(v, "lit") == 0) return;

  /* stamp — record millis() */
  if (strcmp(v, "stamp") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 1) stateSet(a[0], (int64_t)millis());
    return;
  }

  /* transition — set phase */
  if (strcmp(v, "transition") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 2) stateSet(a[0], evalInt(a[1]));
    return;
  }

  /* calc_reaction — compute reaction time */
  if (strcmp(v, "calc_reaction") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 2) {
      int64_t go = stateInt(a[1]);
      stateSet(a[0], (int64_t)(millis() - (unsigned long)go));
    }
    return;
  }

  /* add — state += value */
  if (strcmp(v, "add") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 2) {
      int64_t curr = stateInt(a[0]);
      int64_t delta = 0;
      if (a[1].is<JsonObject>()) {
        JsonObject inner = a[1];
        const char* innerV = inner["v"];
        if (innerV && strcmp(innerV, "lit") == 0) {
          delta = evalInt(inner["args"][0]);
        } else if (innerV && strcmp(innerV, "noise") == 0) {
          int lo = (int)evalFloat(inner["args"][0]);
          int hi = (int)evalFloat(inner["args"][1]);
          delta = random(lo, hi);
        } else {
          delta = evalInt(inner["args"][0]);
        }
      } else {
        delta = evalInt(a[1]);
      }
      stateSet(a[0], curr + delta);
    }
    return;
  }

  /* sub — state -= value */
  if (strcmp(v, "sub") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 2) {
      stateSet(a[0], stateInt(a[0]) - evalInt(a[1]));
    }
    return;
  }

  /* toggle — flip bool */
  if (strcmp(v, "toggle") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 1) {
      String key = stripState(a[0]);
      bool val = (bool)(stateRoot[key.c_str()] | false);
      stateRoot[key.c_str()] = !val;
    }
    return;
  }

  /* if — conditional: args=[state_expr, predicate_name] */
  if (strcmp(v, "if") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 2) {
      const char* condExpr = a[0].as<const char*>();
      const char* predExpr = a[1].as<const char*>();
      if (condExpr && predExpr) {
        String combined = String(predExpr) + "(" + String(condExpr) + ")";
        bool cond = evalBool(combined.c_str());
        if (cond) {
          JsonArray thenBlock = verb["then"];
          if (thenBlock) for (JsonObject sv : thenBlock) execVerb(sv);
        } else {
          JsonArray elseBlock = verb["else"];
          if (elseBlock) for (JsonObject sv : elseBlock) execVerb(sv);
        }
      }
    }
    return;
  }

  /* render_if — conditional render by phase match */
  if (strcmp(v, "render_if") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 2) {
      if (stateInt(a[0]) == evalInt(a[1])) {
        JsonArray thenBlock = verb["then"];
        if (thenBlock) for (JsonObject sv : thenBlock) execVerb(sv);
      }
    }
    return;
  }

  /* render — draw primitive */
  if (strcmp(v, "render") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 1) renderPrimitive(a[0], a[1]);
    return;
  }

  /* graph — temperature line graph drawing (temp-logger) */
  if (strcmp(v, "graph") == 0) {
    JsonArray a = verb["args"];
    if (a && a.size() >= 1) {
#define GRAPH_SIZE 100
      static float hist[GRAPH_SIZE];
      static int   hCnt = 0;
      float currentTemp = stateFloat(a[0]);
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
    return;
  }

  /* noise — random (consumed by add verb args) */
  if (strcmp(v, "noise") == 0) return;

  /* unknown — skip */
  return;
}

/* ==========================================================
   Block runner
   ========================================================== */
void execBlock(JsonArray block) {
  if (!block) return;
  for (JsonObject sv : block) execVerb(sv);
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
  behaviorLoaded = true;
  toolChanged = true;
  const char* id = behaviorRoot["id"];
  if (id) { activeToolId = String(id); saveConfigStr("active_tool", id); }
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

void loadBehavior(const char* toolId) {
  Serial.print("Load: "); Serial.println(toolId);
  if (wifiStationOk && fetchBehavior(toolId)) { Serial.println("OK store"); return; }
  if (loadCachedBehavior(toolId)) { Serial.println("OK cache"); return; }
  Serial.println("No behavior"); behaviorLoaded = false;
}

/* ==========================================================
   Built-in default behavior JSONs (flash/PROGMEM)
   ========================================================== */
const char TIMER_JSON[] PROGMEM =
  "{\"id\":\"timer\",\"name\":\"Count-Up Timer\",\"layout\":\"fullscreen\","
  "\"state\":[{\"name\":\"elapsed_ms\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"running\",\"type\":\"bool\",\"init\":true}],"
  "\"on_tick\":\"tick\",\"on_touch\":\"on_touch\","
  "\"behaviors\":{\"tick\":[{\"v\":\"if\",\"args\":[\"state.running\",\"is_true\"],"
  "\"then\":[{\"v\":\"add\",\"args\":[\"state.elapsed_ms\",{\"v\":\"lit\",\"args\":\"1000\"}]},"
  "{\"v\":\"render\",\"args\":[\"clear\",{\"color\":\"black\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":25,\"y\":60,\"src\":\"Timer\",\"font\":2,\"color\":\"yellow\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":50,\"y\":110,\"src\":\"format_ms(state.elapsed_ms)\",\"font\":6,\"color\":\"green\"}]},"
  "{\"v\":\"render\",\"args\":[\"bar\",{\"x\":10,\"y\":170,\"w\":\"progress_bar(state.elapsed_ms)\",\"h\":10,\"color\":\"blue\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":30,\"y\":210,\"src\":\"Touch to pause/resume\",\"font\":2,\"color\":\"white\"}]}]}],"
  "\"on_touch\":[{\"v\":\"toggle\",\"args\":[\"state.running\"]}]}}";

const char TEMP_JSON[] PROGMEM =
  "{\"id\":\"temp-logger\",\"name\":\"Temp Logger\",\"layout\":\"fullscreen\","
  "\"state\":[{\"name\":\"temp\",\"type\":\"float\",\"init\":72.0},"
  "{\"name\":\"last_tick\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"sample_idx\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"sample_cnt\",\"type\":\"int64\",\"init\":0}],"
  "\"on_tick\":\"tick\","
  "\"behaviors\":{\"tick\":[{\"v\":\"if\",\"args\":[\"state.last_tick\",\"stale_250ms\"],\"then\":["
  "{\"v\":\"stamp\",\"args\":[\"state.last_tick\"]},"
  "{\"v\":\"add\",\"args\":[\"state.temp\",{\"v\":\"noise\",\"args\":[\"-2.0\",\"2.0\"]}]},"
  "{\"v\":\"if\",\"args\":[\"state.temp\",\"clamp_60_90\"],\"then\":[]},"
  "{\"v\":\"render\",\"args\":[\"clear\",{\"color\":\"black\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":80,\"y\":40,\"src\":\"Temp Logger\",\"font\":2,\"color\":\"yellow\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":70,\"y\":70,\"src\":\"format_temp(state.temp)\",\"font\":4,\"color\":\"cyan\"}]},"
  "{\"v\":\"render\",\"args\":[\"rect\",{\"x\":10,\"y\":100,\"w\":220,\"h\":180,\"color\":\"darkgrey\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":215,\"y\":100,\"src\":\"90F\",\"font\":1,\"color\":\"white\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":215,\"y\":270,\"src\":\"60F\",\"font\":1,\"color\":\"white\"}]},"
  "{\"v\":\"graph\",\"args\":[\"state.temp\",{}]},"
  "{\"v\":\"add\",\"args\":[\"state.sample_idx\",{\"v\":\"lit\",\"args\":\"1\"}]},"
  "{\"v\":\"if\",\"args\":[\"state.sample_cnt\",\"lt_100\"],\"then\":["
  "{\"v\":\"add\",\"args\":[\"state.sample_cnt\",{\"v\":\"lit\",\"args\":\"1\"}]}]}]}]}}";

const char REACTION_JSON[] PROGMEM =
  "{\"id\":\"reaction-game\",\"name\":\"Reaction Game\",\"layout\":\"fullscreen\","
  "\"state\":[{\"name\":\"phase\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"wait_start\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"go_at\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"delay_ms\",\"type\":\"int64\",\"init\":3000},"
  "{\"name\":\"touch_ms\",\"type\":\"int64\",\"init\":0},"
  "{\"name\":\"show_until\",\"type\":\"int64\",\"init\":0}],"
  "\"on_tick\":\"tick\",\"on_touch\":\"on_touch\","
  "\"behaviors\":{\"tick\":["
  "{\"v\":\"render_if\",\"args\":[\"state.phase\",\"0\"],\"then\":["
  "{\"v\":\"render\",\"args\":[\"clear\",{\"color\":\"black\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":70,\"y\":40,\"src\":\"Reaction Game\",\"font\":2,\"color\":\"yellow\"}]},"
  "{\"v\":\"render\",\"args\":[\"circle\",{\"x\":120,\"y\":140,\"r\":50,\"color\":\"green\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":65,\"y\":200,\"src\":\"READY...\",\"font\":2,\"color\":\"white\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":30,\"y\":240,\"src\":\"(wait for GO)\",\"font\":2,\"color\":\"darkgrey\"}]},"
  "{\"v\":\"stamp\",\"args\":[\"state.wait_start\"]},"
  "{\"v\":\"transition\",\"args\":[\"state.phase\",\"1\"]}]},"
  "{\"v\":\"render_if\",\"args\":[\"state.phase\",\"1\"],\"then\":["
  "{\"v\":\"if\",\"args\":[\"state.wait_start\",\"delay_elapsed\"],\"then\":["
  "{\"v\":\"transition\",\"args\":[\"state.phase\",\"2\"]},"
  "{\"v\":\"stamp\",\"args\":[\"state.go_at\"]},"
  "{\"v\":\"render\",\"args\":[\"clear\",{\"color\":\"black\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":70,\"y\":40,\"src\":\"Reaction Game\",\"font\":2,\"color\":\"yellow\"}]},"
  "{\"v\":\"render\",\"args\":[\"circle\",{\"x\":120,\"y\":140,\"r\":50,\"color\":\"red\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":75,\"y\":210,\"src\":\"TAP!\",\"font\":2,\"color\":\"white\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":30,\"y\":240,\"src\":\"Touch the screen!\",\"font\":2,\"color\":\"white\"}]}]}]},"
  "{\"v\":\"render_if\",\"args\":[\"state.phase\",\"2\"],\"then\":["
  "{\"v\":\"if\",\"args\":[\"state.show_until\",\"timeout_3s\"],\"then\":["
  "{\"v\":\"transition\",\"args\":[\"state.phase\",\"0\"]}]}]}],"
  "\"on_touch\":[{\"v\":\"render_if\",\"args\":[\"state.phase\",\"2\"],\"then\":["
  "{\"v\":\"calc_reaction\",\"args\":[\"state.touch_ms\",\"state.go_at\"]},"
  "{\"v\":\"render\",\"args\":[\"clear\",{\"color\":\"black\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":70,\"y\":40,\"src\":\"Reaction Game\",\"font\":2,\"color\":\"yellow\"}]},"
  "{\"v\":\"render\",\"args\":[\"text\",{\"x\":40,\"y\":110,\"src\":\"format_ms(state.touch_ms)\",\"font\":6,\"color\":\"green\"}]},"
  "{\"v\":\"stamp\",\"args\":[\"state.show_until\"]}]}]}}";

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
    saveConfigStr("store_url", storeUrl.c_str());
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
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Stage 2 booting...", 10, 10, 2);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount fail — formatting");
    LittleFS.format();
    LittleFS.begin();
  }
  LittleFS.mkdir("/behaviors/");

  storeUrl = loadConfigStr("store_url", DEFAULT_STORE);
  activeToolId = loadConfigStr("active_tool", DEFAULT_TOOL);

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
  Serial.println("OneBoard Stage 2 ready");
  Serial.print("AP:  "); Serial.println(SOFT_AP_SSID);
  Serial.print("IP:  "); Serial.println(WiFi.softAPIP());
  if (wifiStationOk) { Serial.print("STA: "); Serial.println(WiFi.localIP()); }
  Serial.print("Store: "); Serial.println(storeUrl);
  Serial.print("Tool:  "); Serial.println(activeToolId);

  delay(1500);
  toolChanged = true;
}

/* ==========================================================
   Main loop
   ========================================================== */
void loop() {
  server.handleClient();

  /* Periodic tick */
  unsigned long now = millis();
  if (now - lastTickMs >= TICK_MS) {
    lastTickMs = now;
    if (behaviorLoaded && behaviorRoot["on_tick"]) {
      const char* blockName = behaviorRoot["on_tick"];
      JsonArray block = behaviorRoot["behaviors"][blockName];
      if (block) execBlock(block);
    }
  }

  /* Touch handling */
  uint16_t tx = 0, ty = 0;
  bool touched = tft.getTouch(&tx, &ty);
  if (touched && behaviorLoaded && behaviorRoot["on_touch"]) {
    const char* blockName = behaviorRoot["on_touch"];
    JsonArray block = behaviorRoot["behaviors"][blockName];
    if (block) execBlock(block);
  }

  delay(LOOP_DELAY);
}