/* ==========================================================
   OneBoard Stage 3 — Real VM core (compiled behavior instructions)
   Behavior JSON compiled to compact structs at load time;
   runtime executes from VmProgram structs, not raw JSON.
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

#include "vm.h"  // Stage 3 compiled VM

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
#define SOFT_AP_SSID    "OneBoard-Stage3"
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
#define JSON_CAPACITY   4096

/* ==========================================================
   Globals
   ========================================================== */
TFT_eSPI      tft = TFT_eSPI();
WebServer     server(80);
Preferences   prefs;
bool          wifiStationOk = false;

String        activeToolId = DEFAULT_TOOL;
String        storeUrl     = DEFAULT_STORE;

// Stage 3 compiled VM — program + runtime state
VmProgram     g_prog;
VmState       g_state;
bool          vmInitialized = false;

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
   Behavior loading — parse JSON → compile to VM program
   ========================================================== */
// Forward declarations for PROGMEM JSON arrays (defined below)
extern const char TIMER_JSON[] PROGMEM;
extern const char TEMP_JSON[] PROGMEM;
extern const char REACTION_JSON[] PROGMEM;
extern const char TAP_COUNTER_JSON[] PROGMEM;

const char* progmemBehavior(const char* toolId) {
  if (!toolId) return NULL;
  if (strcmp(toolId, "timer") == 0)         return TIMER_JSON;
  if (strcmp(toolId, "temp-logger") == 0)   return TEMP_JSON;
  if (strcmp(toolId, "reaction-game") == 0) return REACTION_JSON;
  if (strcmp(toolId, "tap-counter") == 0)   return TAP_COUNTER_JSON;
  return NULL;
}

bool loadBehaviorFromJson(const char* jsonStr) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) { Serial.print("JSON err: "); Serial.println(err.c_str()); return false; }

  JsonObject root = doc.as<JsonObject>();
  if (!root["id"]) { Serial.println("No 'id'"); return false; }

  // Compile JSON → compact VM instructions
  if (!vmCompile(root, &g_prog)) { Serial.println("VM compile fail"); return false; }

  // Initialize runtime state from compiled program
  vmStateInit(&g_state, &g_prog);
  vmInitialized = true;

  // Draw the initial screen immediately — a behavior should render on load,
  // not wait for its first scheduled tick (and static screens shouldn't need
  // a tick at all). Mark the first matching handler's renders dirty and flush.
  vmMarkInitialRender(&g_state, &g_prog);
  vmRender(&g_state, &g_prog, &tft);

  const char* id = root["id"];
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
    return loadBehaviorFromJson(payload.c_str());
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
  return loadBehaviorFromJson(jsonStr.c_str());
}

void loadBehavior(const char* toolId) {
  Serial.print("Load: "); Serial.println(toolId);
  vmInitialized = false;
  if (wifiStationOk && fetchBehavior(toolId)) { Serial.println("OK store"); return; }
  if (loadCachedBehavior(toolId)) { Serial.println("OK cache"); return; }
  const char* builtin = progmemBehavior(toolId);
  if (builtin && loadBehaviorFromJson(builtin)) { Serial.println("OK builtin (PROGMEM)"); return; }
  Serial.println("No behavior");
}

void seedDefaultBehaviors() {
  const char* ids[4] = {"timer", "temp-logger", "reaction-game", "tap-counter"};
  const char* js[4]  = {TIMER_JSON, TEMP_JSON, REACTION_JSON, TAP_COUNTER_JSON};
  for (int i = 0; i < 4; i++) {
    String path = String(BEHAVIOR_DIR) + String(ids[i]) + ".json";
    if (!LittleFS.exists(path.c_str())) {
      File f = LittleFS.open(path.c_str(), "w");
      if (f) { f.print(js[i]); f.close(); Serial.print("Seed: "); Serial.println(path); }
    }
  }
}
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
  "{\"id\":\"temp-logger\",\"name\":\"Temp Logger\",\"layout\":\"fullscreen\",\"state\":[{\"name\":\"temp\",\"type\":\"float\",\"init\":72.0},{\"name\":\"last_tick\",\"type\":\"int64\",\"init\":0},{\"name\":\"sample_idx\",\"type\":\"int64\",\"init\":0},{\"name\":\"sample_cnt\",\"type\":\"int64\",\"init\":0},{\"name\":\"temp_txt\",\"type\":\"string\",\"init\":\"72.0 F\"}],\"events\":[{\"name\":\"tick\",\"schedule\":{\"type\":\"interval\",\"ms\":250},\"handlers\":[{\"guard\":{\"op\":\"always\"},\"run\":[{\"stamp\":{\"slot\":\"last_tick\"}},{\"add\":{\"slot\":\"temp\",\"delta\":{\"noise\":{\"lo\":-2.0,\"hi\":2.0}}}},{\"if\":{\"guard\":{\"op\":\"lt\",\"a\":{\"slot\":\"temp\"},\"b\":{\"lit\":60}},\"then\":[{\"setf\":{\"slot\":\"temp\",\"value\":{\"lit\":60}}}]}},{\"if\":{\"guard\":{\"op\":\"gt\",\"a\":{\"slot\":\"temp\"},\"b\":{\"lit\":90}},\"then\":[{\"setf\":{\"slot\":\"temp\",\"value\":{\"lit\":90}}}]}},{\"format\":{\"slot\":\"temp_txt\",\"op\":\"temp\",\"src\":{\"slot\":\"temp\"}}},{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":80},\"y\":{\"lit\":40},\"src\":{\"lit\":\"Temp Logger\"},\"font\":{\"lit\":2},\"color\":\"yellow\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":70},\"y\":{\"lit\":70},\"src\":{\"slot\":\"temp_txt\"},\"font\":{\"lit\":4},\"color\":\"cyan\"}},{\"render\":{\"type\":\"rect\",\"x\":{\"lit\":10},\"y\":{\"lit\":100},\"w\":{\"lit\":220},\"h\":{\"lit\":180},\"color\":\"darkgrey\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":215},\"y\":{\"lit\":100},\"src\":{\"lit\":\"90F\"},\"font\":{\"lit\":1},\"color\":\"white\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":215},\"y\":{\"lit\":270},\"src\":{\"lit\":\"60F\"},\"font\":{\"lit\":1},\"color\":\"white\"}},{\"render\":{\"type\":\"graph\",\"slot\":{\"slot\":\"temp\"}}},{\"add\":{\"slot\":\"sample_idx\",\"delta\":{\"lit\":1}}},{\"if\":{\"guard\":{\"op\":\"lt\",\"a\":{\"slot\":\"sample_cnt\"},\"b\":{\"lit\":100}},\"then\":[{\"add\":{\"slot\":\"sample_cnt\",\"delta\":{\"lit\":1}}}]}}]}]}]}";

const char REACTION_JSON[] PROGMEM =
  "{\"id\":\"reaction-game\",\"name\":\"Reaction Game\",\"layout\":\"fullscreen\",\"state\":[{\"name\":\"phase\",\"type\":\"int64\",\"init\":0},{\"name\":\"wait_start\",\"type\":\"int64\",\"init\":0},{\"name\":\"go_at\",\"type\":\"int64\",\"init\":0},{\"name\":\"delay_ms\",\"type\":\"int64\",\"init\":3000},{\"name\":\"touch_ms\",\"type\":\"int64\",\"init\":0},{\"name\":\"show_until\",\"type\":\"int64\",\"init\":0},{\"name\":\"result_txt\",\"type\":\"string\",\"init\":\"00:00\"}],\"events\":[{\"name\":\"tick\",\"schedule\":{\"type\":\"interval\",\"ms\":100},\"handlers\":[{\"guard\":{\"op\":\"eq\",\"a\":{\"slot\":\"phase\"},\"b\":{\"lit\":0}},\"run\":[{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":70},\"y\":{\"lit\":40},\"src\":{\"lit\":\"Reaction Game\"},\"font\":{\"lit\":2},\"color\":\"yellow\"}},{\"render\":{\"type\":\"circle\",\"x\":{\"lit\":120},\"y\":{\"lit\":140},\"r\":{\"lit\":50},\"color\":\"green\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":65},\"y\":{\"lit\":200},\"src\":{\"lit\":\"READY...\"},\"font\":{\"lit\":2},\"color\":\"white\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":30},\"y\":{\"lit\":240},\"src\":{\"lit\":\"(wait for GO)\"},\"font\":{\"lit\":2},\"color\":\"darkgrey\"}},{\"stamp\":{\"slot\":\"wait_start\"}},{\"set\":{\"slot\":\"phase\",\"value\":{\"lit\":1}}}]},{\"guard\":{\"op\":\"eq\",\"a\":{\"slot\":\"phase\"},\"b\":{\"lit\":1}},\"run\":[{\"if\":{\"guard\":{\"op\":\"elapsed_ge\",\"a\":{\"slot\":\"wait_start\"},\"b\":{\"slot\":\"delay_ms\"}},\"then\":[{\"set\":{\"slot\":\"phase\",\"value\":{\"lit\":2}}},{\"stamp\":{\"slot\":\"go_at\"}},{\"set\":{\"slot\":\"show_until\",\"value\":{\"lit\":0}}},{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":70},\"y\":{\"lit\":40},\"src\":{\"lit\":\"Reaction Game\"},\"font\":{\"lit\":2},\"color\":\"yellow\"}},{\"render\":{\"type\":\"circle\",\"x\":{\"lit\":120},\"y\":{\"lit\":140},\"r\":{\"lit\":50},\"color\":\"red\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":75},\"y\":{\"lit\":210},\"src\":{\"lit\":\"TAP!\"},\"font\":{\"lit\":2},\"color\":\"white\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":30},\"y\":{\"lit\":240},\"src\":{\"lit\":\"Touch the screen!\"},\"font\":{\"lit\":2},\"color\":\"white\"}}]}}]},{\"guard\":{\"op\":\"eq\",\"a\":{\"slot\":\"phase\"},\"b\":{\"lit\":2}},\"run\":[{\"if\":{\"guard\":{\"op\":\"elapsed_ge\",\"a\":{\"slot\":\"show_until\"},\"b\":{\"lit\":3000}},\"then\":[{\"set\":{\"slot\":\"phase\",\"value\":{\"lit\":0}}}]}}]}]},{\"name\":\"touch-down\",\"region\":{\"x\":0,\"y\":0,\"w\":240,\"h\":320},\"handlers\":[{\"guard\":{\"op\":\"eq\",\"a\":{\"slot\":\"phase\"},\"b\":{\"lit\":2}},\"run\":[{\"now_minus\":{\"slot\":\"touch_ms\",\"from\":{\"slot\":\"go_at\"}}},{\"format\":{\"slot\":\"result_txt\",\"op\":\"react\",\"src\":{\"slot\":\"touch_ms\"}}},{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":70},\"y\":{\"lit\":40},\"src\":{\"lit\":\"Reaction Game\"},\"font\":{\"lit\":2},\"color\":\"yellow\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":40},\"y\":{\"lit\":110},\"src\":{\"slot\":\"result_txt\"},\"font\":{\"lit\":6},\"color\":\"green\"}},{\"stamp\":{\"slot\":\"show_until\"}}]}]}]}";

// PROGMEM JSON arrays above — behavior loading uses them via forward declarations.

const char TAP_COUNTER_JSON[] PROGMEM =
  "{\"id\":\"tap-counter\",\"name\":\"Tap Counter\",\"layout\":\"fullscreen\",\"state\":[{\"name\":\"count\",\"type\":\"int64\",\"init\":0},{\"name\":\"count_txt\",\"type\":\"string\",\"init\":\"0\"}],\"events\":[{\"name\":\"touch-down\",\"handlers\":[{\"run\":[{\"add\":{\"slot\":\"count\",\"delta\":{\"lit\":1}}},{\"format\":{\"slot\":\"count_txt\",\"op\":\"int\",\"src\":{\"slot\":\"count\"}}},{\"render\":{\"type\":\"clear\",\"color\":\"black\"}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":120},\"y\":{\"lit\":50},\"src\":{\"lit\":\"Tap Counter\"},\"font\":{\"lit\":2},\"color\":\"yellow\",\"center\":true}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":120},\"y\":{\"lit\":120},\"src\":{\"slot\":\"count_txt\"},\"font\":{\"lit\":6},\"color\":\"green\",\"center\":true}},{\"render\":{\"type\":\"text\",\"x\":{\"lit\":120},\"y\":{\"lit\":240},\"src\":{\"lit\":\"Tap to count\"},\"font\":{\"lit\":2},\"color\":\"white\",\"center\":true}}]}]}]}";

/* ==========================================================
   WiFi helpers
   ========================================================== */
bool wifiStationConnect(bool verbose = true) {
  String ssid = loadConfigStr(WIFI_SSID_KEY, "");
  String pass = loadConfigStr(WIFI_PASS_KEY, "");
  if (ssid.length() == 0) return false;
  if (verbose) { Serial.print("WiFi: "); Serial.println(ssid); }
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) { delay(500); if (verbose) Serial.print("."); }
  if (verbose) Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifiStationOk = true;
    if (verbose) { Serial.print("IP: "); Serial.println(WiFi.localIP()); }
    return true;
  }
  wifiStationOk = false;
  if (verbose) Serial.println("WiFi fail — AP fallback");
  return false;
}

void tryStationConnect() {
  wifiStationConnect(true);
}

/* ==========================================================
   Web server handlers
   ========================================================== */
void handleRoot() {
  String html = F("<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=320'>"
    "<title>OneBoard Stage 3</title>"
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
    "<button id='btn-tap-counter' onclick='loadTool(\"tap-counter\")'>&#128073; Tap Counter</button>"
    "<div style='margin-top:18px;border-top:1px solid #444;padding-top:12px'>"
      "<div style='font-size:0.9em;color:#9cf;margin-bottom:6px'>WiFi (join your home network)</div>"
      "<input id='wifiSsid' type='text' style='width:100%;max-width:280px;padding:8px;background:#222;color:#fff;border:1px solid #555'"
        " placeholder='Home WiFi SSID' />"
      "<input id='wifiPass' type='password' style='width:100%;max-width:280px;padding:8px;background:#222;color:#fff;border:1px solid #555'"
        " placeholder='Home WiFi password' />"
      "<button onclick='setWifi()' style='background:#333;border-color:#9cf'>Join Home WiFi</button>"
      "<div id='wifiStatus' style='font-size:0.8em;color:#888'></div>"
    "</div>"
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
      "async function setWifi(){"
        "var s=document.getElementById('wifiSsid').value.trim();"
        "var p=document.getElementById('wifiPass').value;"
        "if(!s)return;"
        "var st=document.getElementById('wifiStatus');"
        "st.innerText='Connecting to '+s+'...';"
        "try{"
          "var r=await fetch('/wifi?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p));"
          "var j=await r.json();"
          "if(j.ok){st.innerText='Joined '+s+' — IP: '+j.ip+' (reach the board at http://'+j.ip+' from now on)'}"
          "else{st.innerText='Failed to join '+s+' — check name/password, AP still up at 192.168.4.1'}"
        "}catch(e){st.innerText='Failed'}"
      "}"
      "fetch('/status').then(r=>r.json()).then(j=>{"
        "document.getElementById('current').innerText='Tool: '+j.tool;"
        "document.getElementById('storeLabel').innerText='Store: '+j.store_url + (j.wifi?' | WiFi IP: '+j.ip:'');"
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

void handleWifi() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    saveConfigStr(WIFI_SSID_KEY, ssid.c_str());
    saveConfigStr(WIFI_PASS_KEY, pass.c_str());
    bool ok = wifiStationConnect(false);
    String ip = ok ? WiFi.localIP().toString() : "";
    String json = "{\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"ssid\":\"" + ssid + "\"";
    json += ",\"ip\":\"" + ip + "\"}";
    server.send(200, "application/json", json);
    return;
  }
  server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid/pass\"}");
}

void handleStatus() {
  String json = "{\"tool\":\"";
  json += activeToolId;
  json += "\",\"store_url\":\"";
  json += storeUrl;
  json += "\",\"loaded\":";
  json += vmInitialized ? "true" : "false";
  json += ",\"wifi\":";
  json += wifiStationOk ? "true" : "false";
  json += ",\"ip\":\"";
  json += wifiStationOk ? WiFi.localIP().toString() : "";
  json += "\"}";
  server.send(200, "application/json", json);
}

/* ==========================================================
   /schema — self-description: the VM vocabulary an LLM needs
   to author a valid behavior definition for this board.
   ========================================================== */
void handleSchema() {
  const char* schema = "{"
    "\"board\":\"CYD ESP32-2432S028\",\"display\":\"240x320\","
    "\"slots\":{\"types\":[\"int64\",\"float\",\"bool\",\"string\"],\"max\":12},"
    "\"verbs\":[\"add\",\"sub\",\"set\",\"setf\",\"toggle\",\"stamp\","
      "\"now_minus\",\"format\",\"progress\",\"render\",\"if\"],"
    "\"guards\":[\"eq\",\"ne\",\"lt\",\"le\",\"gt\",\"ge\",\"is_true\","
      "\"elapsed_ge\",\"always\"],"
    "\"render_types\":[\"clear\",\"text\",\"rect\",\"fillrect\",\"line\","
      "\"circle\",\"bar\",\"graph\"],"
    "\"format_ops\":[\"ms\",\"temp\",\"react\",\"int\"],"
    "\"colors\":[\"black\",\"white\",\"red\",\"green\",\"blue\",\"yellow\","
      "\"cyan\",\"magenta\",\"orange\",\"navy\",\"darkgrey\",\"darkgray\","
      "\"grey\",\"gray\"],"
    "\"fonts\":[1,2,3,4,5,6,7],"
    "\"limits\":{\"events\":6,\"handlers_per_event\":6,\"slots\":12,"
      "\"renders\":20,\"instructions\":80,\"if\":\"forward-only\"},"
    "\"conventions\":[\"literal values as {\\\"lit\\\":N}\","
      "\"state refs as {\\\"slot\\\":\\\"name\\\"}\","
      "\"first-match-guard-wins per event\",\"touch event named touch-down\","
      "\"every screen render set starts with a clear\","
      "\"see docs/BEHAVIOR-AUTHORING.md + tools/validate_behavior.py\"]"
    "}";
  server.send(200, "application/json", schema);
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
  tft.drawString("Stage 3 booting...", 10, 10, 2);

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
  server.on("/schema",  handleSchema);
  server.on("/store",   handleStore);
  server.on("/wifi",    handleWifi);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println();
  Serial.println("OneBoard Stage 3 ready (compiled VM)");
  Serial.print("AP:  "); Serial.println(SOFT_AP_SSID);
  Serial.print("IP:  "); Serial.println(WiFi.softAPIP());
  if (wifiStationOk) { Serial.print("STA: "); Serial.println(WiFi.localIP()); }
  Serial.print("Store: "); Serial.println(storeUrl);
  Serial.print("Tool:  "); Serial.println(activeToolId);
}

/* ==========================================================
   Main loop — VM-driven
   ========================================================== */
void loop() {
  server.handleClient();
  unsigned long now = millis();

  if (vmInitialized) {
    // Run scheduled events (ticks); redraw only when something actually ran
    // (Stage 2 behavior — a tick or touch re-renders; no idle redraw storm).
    bool ran = vmTick(&g_state, &g_prog, now);
    if (ran) vmRender(&g_state, &g_prog, &tft);
  }

  /* Touch → named event "touch-down" (edge-detected, 250ms cooldown).
     Touch region matching is done by the behavior author in JSON,
     but since all current behaviors use full-screen touch-down,
     we dispatch unconditionally and let handlers' guards filter. */
  static bool touchWasDown = false;
  static uint32_t lastTouchEventMs = 0;
  uint16_t tx = 0, ty = 0;
  bool touched = touchReadScreen(&tx, &ty);
  uint32_t tNow = millis();
  if (touched && !touchWasDown && (tNow - lastTouchEventMs > 250)) {
    lastTouchEventMs = tNow;
    if (vmInitialized) {
      if (vmDispatch(&g_state, &g_prog, "touch-down"))
        vmRender(&g_state, &g_prog, &tft);
    }
  }
  touchWasDown = touched;

  delay(LOOP_DELAY);
}