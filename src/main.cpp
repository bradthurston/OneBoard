/* ==========================================================
   OneBoard Stage 3 — Real VM core (compiled behavior instructions)
   Behavior JSON compiled to compact structs at load time;
   runtime executes from VmProgram structs, not raw JSON.
   ========================================================== */
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

#include "vm.h"  // Stage 3 compiled VM

/* ==========================================================
   Configuration — board variant. Everything board-specific is overridable
   via build_flags so the same firmware serves the common CYD family. The
   verified default is the ESP32-2432S028R. See VARIANTS.md for
   the pin table + sources.
   ========================================================== */
#ifndef TFT_BL
#define TFT_BL          21      /* S028: 21 | S024: 27 | S032: 21 or 27 */
#endif

/* Touch bus topology (this is the big fork in the CYD family):
   0 = separate SPI bus (S028R verified): touch on its own VSPI, pins below.
   1 = shared with the display bus (S024 / some S032): touch CS on the same
       MISO/MOSI/SCLK as the TFT; the XPT2046 talks on the display's bus.
   NOTE: mode 1 is built from vendor/community schematics but NOT hardware-
   verified here — the S028R bench board is separate-bus only. Test on real
   hardware before trusting it. */
#ifndef TOUCH_SHARED_BUS
#define TOUCH_SHARED_BUS    0
#endif
#ifndef TOUCH_CS
#define TOUCH_CS            33
#endif
#ifndef TOUCH_IRQ
#define TOUCH_IRQ           36      /* S028 separate-bus IRQ */
#endif
#ifndef TOUCH_SCLK
#define TOUCH_SCLK          25      /* S028 separate-bus clock */
#endif
#ifndef TOUCH_MOSI
#define TOUCH_MOSI          32      /* S028 separate-bus MOSI */
#endif
#ifndef TOUCH_MISO
#define TOUCH_MISO          39      /* S028 separate-bus MISO */
#endif

#define SOFT_AP_SSID    "OneBoard-Stage3"
#define SOFT_AP_PSK     ""

/* ==========================================================
   XPT2046 touch — SEPARATE SPI instance (VSPI), independent of the TFT (HSPI).
   This board (2432S028R / E32R28T): SCLK25 MOSI32 MISO39 CS33 IRQ36.
   Uses the stock XPT2046_Touchscreen library on its own VSPI bus — the SAME
   proven profile the working firmwares (Bruce / Wireless Wizard)
   use. Requires USE_HSPI_PORT in build flags so the display owns HSPI and the
   touch owns VSPI (without it both fight for VSPI and touch never reads).
   ========================================================== */
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);   // CS, IRQ

/* Declared early because touchBegin() (shared-bus mode) needs the display's
   SPI instance. Initialized in setup(). */
TFT_eSPI tft = TFT_eSPI();

void touchBegin() {
  if (TOUCH_SHARED_BUS) {
    /* Touch shares the display's SPI (same MISO/MOSI/SCLK as the TFT) —
       S024 / some S032. We hand the display SPI instance to the XPT2046
       instead of a private VSPI bus. */
    touch.begin(tft.getSPIinstance());
  } else {
    /* S028R (verified): touch on its own VSPI bus, independent of the TFT's
       HSPI. Requires USE_HSPI_PORT in build flags so the display owns HSPI
       and the touch owns VSPI (without it both fight for VSPI and touch
       never reads). */
    touchSPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    touch.begin(touchSPI);
  }
  touch.setRotation(0);   /* portrait 240x320 — matches the tool definitions */
}

/* Touch calibration — measured on THIS board (2026-10-16, /touch corner taps).
   Raw X→screen-x and raw Y→screen-y directly (NO axis swap): a tap at
   top-left gives raw [289,219], bottom-right gives [3740,3685]. The usable
   raw range (~289..3740 X, ~219..3685 Y) is far narrower than 0..4095, so a
   naive raw*240/4096 map is offset. Full-screen tools masked it; the
   row-accurate home list exposed it (taps landed on the wrong tool). */
#define TX_MIN 289
#define TX_MAX 3740
#define TY_MIN 219
#define TY_MAX 3685

/* Raw touch state (last read), exposed via /touch */
volatile int  lastRawX = -1, lastRawY = -1, lastRawZ = 0;
// Ring buffer of recent raw taps (i, i indexes), so /touch can return all four
// calibration corners in one shot instead of only the last tap.
#define TOUCH_BUF 16
volatile int  tapBufX[TOUCH_BUF], tapBufY[TOUCH_BUF], tapBufZ[TOUCH_BUF];
volatile int  tapBufN = 0;

/* Returns true + screen coords (portrait 240x320) on press. Stock lib does the
   heavy lifting (Z1/Z2 pressure, oversampling, power-down). We ALSO stash the
   raw sensor point so /touch can report it for calibration.
   NOTE: the current mapping (raw*240/4096) is a linear placeholder — Brad
   reported home-list taps land on the wrong tool, which is a touch OFFSET,
   not a tool bug. Full-screen tools mask it; accurate row taps expose it.
   To fix: sample /touch at the four corners, derive the real per-axis bounds
   + axis rotation, then replace this mapping (see wiki CYD Reference: this
   sensor is ~X 200..3700, Y 240..3800, not 0..4095). */
static bool touchReadScreen(uint16_t *sx, uint16_t *sy) {
  // held reset: when the panel reports no press, the prior press ended.
  static bool held = false;
  if (!(touch.tirqTouched() && touch.touched())) { held = false; return false; }
  TS_Point p = touch.getPoint();
  if (p.z < 300) { held = false; return false; }
  lastRawX = p.x; lastRawY = p.y; lastRawZ = p.z;
  // Capture one raw sample per PRESS (rising edge), not per 5ms loop read —
  // a single hold otherwise floods the buffer with jitter and swallows the
  // other calibration corners. held tracks press state.
  if (!held) {
    if (tapBufN < TOUCH_BUF) {
      tapBufX[tapBufN] = p.x; tapBufY[tapBufN] = p.y; tapBufZ[tapBufN] = p.z;
      tapBufN++;
    }
    held = true;
  }
  // Calibrated linear mapping: raw X/Y sweep the full screen across the
  // measured usable range, clamping to edges.
  int32_t rx = ((int32_t)p.x - TX_MIN) * 240 / (TX_MAX - TX_MIN);
  int32_t ry = ((int32_t)p.y - TY_MIN) * 320 / (TY_MAX - TY_MIN);
  if (rx < 0) rx = 0; else if (rx > 239) rx = 239;
  if (ry < 0) ry = 0; else if (ry > 319) ry = 319;
  *sx = (uint16_t)rx;
  *sy = (uint16_t)ry;
  return true;
}

/* ==========================================================
   Configuration
   ========================================================== */

/* NVS namespaces/keys */
#define CFG_NS          "obm_cfg"
#define WIFI_SSID_KEY   "w_ssid"
#define WIFI_PASS_KEY   "w_pass"
#define STORE_KEY       "store_url"
#define STORE_AUTO_KEY  "store_auto"
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
WebServer     server(80);
DNSServer     dnsServer;
Preferences   prefs;
bool          wifiStationOk = false;
bool          provisioningMode = false;  // no WiFi saved yet — captive portal up

String        activeToolId = DEFAULT_TOOL;
String        storeUrl     = DEFAULT_STORE;
// When true (the default) the board browses mDNS for a store on every boot and
// uses whatever it finds, so nobody has to type an address. Setting a URL by
// hand turns this off — an explicit choice should win.
bool          storeAuto    = true;
bool          storeFound   = false;   // did discovery find one this boot?
bool          mdnsStarted  = false;

/* ==========================================================
   Store discovery — find the store instead of being told where it is.

   A fresh board's compiled store address (DEFAULT_STORE) points at an AP-mode
   address that cannot exist once the board joins a real network. Without this,
   every new user has to hand-type an IP into the setup form and gets nothing
   until they do.

   So the store advertises itself as _oneboard-store._tcp.local (see
   store/mdns_advertise.py) and the board browses for it. The discovered URL is
   deliberately NOT persisted: looking it up fresh on every boot means a store
   host whose DHCP address moved gets re-found automatically instead of silently
   breaking.

   queryService() blocks for up to ~3s, so this runs at boot and from explicit
   endpoints — never from a hot path.
   ========================================================== */
static bool ensureMdns() {
  if (mdnsStarted) return true;
  if (!WiFi.isConnected()) return false;     // nothing to browse from
  mdnsStarted = MDNS.begin("oneboard");      // also gives us oneboard.local
  return mdnsStarted;
}

// "http://ip:port" for the first store that answers, or "" if none do.
String discoverStore() {
  if (!ensureMdns()) return "";
  int n = MDNS.queryService("oneboard-store", "tcp");
  if (n <= 0) return "";
  String ip = MDNS.IP(0).toString();
  uint16_t p = MDNS.port(0);
  if (ip.length() == 0 || ip == "0.0.0.0") return "";
  if (p == 0) p = 8080;
  return "http://" + ip + ":" + String(p);
}

// Adopt a discovered store. Returns true if one answered.
bool applyStoreDiscovery() {
  if (!storeAuto) return false;
  String url = discoverStore();
  if (url.length() == 0) { storeFound = false; return false; }
  storeUrl   = url;
  storeFound = true;
  Serial.print("Store found: "); Serial.println(storeUrl);
  return true;
}

// Stage 3 compiled VM — program + runtime state
VmProgram     g_prog;
VmState       g_state;
bool          vmInitialized = false;
String        lastLoadError = "";   // human-readable reason for the last failed load

/* ==========================================================
   Home screen — firmware-level "exit out of a tool" affordance.
   A small persistent home button is drawn top-left on EVERY screen
   (even over a running tool) and its touch is intercepted BEFORE
   it reaches the running behavior, so a full-screen tool can never
   hijack it. Tapping it opens a live list of tools from the store;
   tap a tool to load + run it. Not a behavior (behaviors are static
   JSON and can't enumerate the tool list — firmware already queries
   the store for the web page, so it lists tools the same way).
   ========================================================== */
bool          homeMode = false;      // true = showing the home/tool menu
#define HOME_BTN_X 0                 // top-left corner: reserved for home
#define HOME_BTN_Y 0
// Touch hitbox stays a comfortable ~24px even though the drawn arrow is tiny
// (~11px), so the icon looks small but is still easy to hit.
#define HOME_BTN_W 24
#define HOME_BTN_H 24
#define HOME_LIST_MAX 24             // tools we can list (store + builtins)
#define HOME_ROW_H 30                // vertical pitch of each tool row
#define HOME_LIST_Y 44               // first row's y (below title)
String        homeList[HOME_LIST_MAX];
int           homeCount = 0;
int           homeScroll = 0;        // rows scrolled off the top (0 = first)
#define HOME_ROWS_VISIBLE ((320 - HOME_LIST_Y) / HOME_ROW_H)

// Forward decl — loadBehaviorFromJson (defined later) calls this to keep the
// home button on top of every freshly-rendered tool screen.
void drawHomeButton();

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
  if (err) {
    Serial.print("JSON err: "); Serial.println(err.c_str());
    lastLoadError = "invalid JSON: " + String(err.c_str());
    return false;
  }

  JsonObject root = doc.as<JsonObject>();
  if (!root["id"]) {
    Serial.println("No 'id'");
    lastLoadError = "behavior has no 'id'";
    return false;
  }

  // Compile into a TEMP program first — a failed compile must NOT clobber
  // the currently-running behavior (which would blank the screen and kill
  // touch). Only swap the new program in on success.
  VmProgram newProg;
  if (!vmCompile(root, &newProg)) {
    Serial.print("VM compile fail: ");
    Serial.println(newProg.compile_error);
    lastLoadError = newProg.compile_error;
    return false;   // g_prog / g_state / vmInitialized untouched — old tool keeps running
  }

  // Success — commit the new program
  g_prog = newProg;
  vmStateInit(&g_state, &g_prog);
  vmInitialized = true;

  // Draw the initial screen immediately — a behavior should render on load,
  // not wait for its first scheduled tick (and static screens shouldn't need
  // a tick at all). Mark the first matching handler's renders dirty and flush.
  vmMarkInitialRender(&g_state, &g_prog);
  vmRender(&g_state, &g_prog, &tft);
  drawHomeButton();

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

bool loadBehavior(const char* toolId) {
  Serial.print("Load: "); Serial.println(toolId);
  lastLoadError = "";
  // NOTE: do NOT set vmInitialized=false here. A failed load must leave the
  // currently-running behavior alive (loadBehaviorFromJson compiles into a
  // temp program and only commits on success). Setting it false early would
  // blank the screen + kill touch on any bad load.
  if (wifiStationOk && fetchBehavior(toolId)) { Serial.println("OK store"); return true; }
  if (loadCachedBehavior(toolId)) { Serial.println("OK cache"); return true; }
  const char* builtin = progmemBehavior(toolId);
  if (builtin && loadBehaviorFromJson(builtin)) { Serial.println("OK builtin (PROGMEM)"); return true; }
  Serial.println("No behavior");
  if (lastLoadError == "") lastLoadError = "no behavior source found for '" + String(toolId) + "'";
  return false;
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
  "{\n"
  " \"id\": \"temp-logger\",\n"
  " \"name\": \"Temp Logger\",\n"
  " \"layout\": \"fullscreen\",\n"
  " \"state\": [\n"
  "  {\n"
  "   \"name\": \"temp\",\n"
  "   \"type\": \"float\",\n"
  "   \"init\": 72.0\n"
  "  },\n"
  "  {\n"
  "   \"name\": \"last_tick\",\n"
  "   \"type\": \"int64\",\n"
  "   \"init\": 0\n"
  "  },\n"
  "  {\n"
  "   \"name\": \"sample_idx\",\n"
  "   \"type\": \"int64\",\n"
  "   \"init\": 0\n"
  "  },\n"
  "  {\n"
  "   \"name\": \"sample_cnt\",\n"
  "   \"type\": \"int64\",\n"
  "   \"init\": 0\n"
  "  },\n"
  "  {\n"
  "   \"name\": \"temp_txt\",\n"
  "   \"type\": \"string\",\n"
  "   \"init\": \"72.0 F\"\n"
  "  }\n"
  " ],\n"
  " \"events\": [\n"
  "  {\n"
  "   \"name\": \"tick\",\n"
  "   \"schedule\": {\n"
  "    \"type\": \"interval\",\n"
  "    \"ms\": 250\n"
  "   },\n"
  "   \"handlers\": [\n"
  "    {\n"
  "     \"guard\": {\n"
  "      \"op\": \"always\"\n"
  "     },\n"
  "     \"run\": [\n"
  "      {\n"
  "       \"stamp\": {\n"
  "        \"slot\": \"last_tick\"\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"add\": {\n"
  "        \"slot\": \"temp\",\n"
  "        \"delta\": {\n"
  "         \"noise\": {\n"
  "          \"lo\": -2.0,\n"
  "          \"hi\": 2.0\n"
  "         }\n"
  "        }\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"if\": {\n"
  "        \"guard\": {\n"
  "         \"op\": \"lt\",\n"
  "         \"a\": {\n"
  "          \"slot\": \"temp\"\n"
  "         },\n"
  "         \"b\": {\n"
  "          \"lit\": 60\n"
  "         }\n"
  "        },\n"
  "        \"then\": [\n"
  "         {\n"
  "          \"setf\": {\n"
  "           \"slot\": \"temp\",\n"
  "           \"value\": {\n"
  "            \"lit\": 60\n"
  "           }\n"
  "          }\n"
  "         }\n"
  "        ]\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"if\": {\n"
  "        \"guard\": {\n"
  "         \"op\": \"gt\",\n"
  "         \"a\": {\n"
  "          \"slot\": \"temp\"\n"
  "         },\n"
  "         \"b\": {\n"
  "          \"lit\": 90\n"
  "         }\n"
  "        },\n"
  "        \"then\": [\n"
  "         {\n"
  "          \"setf\": {\n"
  "           \"slot\": \"temp\",\n"
  "           \"value\": {\n"
  "            \"lit\": 90\n"
  "           }\n"
  "          }\n"
  "         }\n"
  "        ]\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"format\": {\n"
  "        \"slot\": \"temp_txt\",\n"
  "        \"op\": \"temp\",\n"
  "        \"src\": {\n"
  "         \"slot\": \"temp\"\n"
  "        }\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"render\": {\n"
  "        \"type\": \"clear\",\n"
  "        \"color\": \"black\"\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"render\": {\n"
  "        \"type\": \"text\",\n"
  "        \"x\": {\n"
  "         \"lit\": 80\n"
  "        },\n"
  "        \"y\": {\n"
  "         \"lit\": 40\n"
  "        },\n"
  "        \"src\": {\n"
  "         \"lit\": \"Temp Logger\"\n"
  "        },\n"
  "        \"font\": {\n"
  "         \"lit\": 2\n"
  "        },\n"
  "        \"color\": \"yellow\"\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"render\": {\n"
  "        \"type\": \"text\",\n"
  "        \"x\": {\n"
  "         \"lit\": 70\n"
  "        },\n"
  "        \"y\": {\n"
  "         \"lit\": 70\n"
  "        },\n"
  "        \"src\": {\n"
  "         \"slot\": \"temp_txt\"\n"
  "        },\n"
  "        \"font\": {\n"
  "         \"lit\": 4\n"
  "        },\n"
  "        \"color\": \"cyan\"\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"render\": {\n"
  "        \"type\": \"rect\",\n"
  "        \"x\": {\n"
  "         \"lit\": 10\n"
  "        },\n"
  "        \"y\": {\n"
  "         \"lit\": 100\n"
  "        },\n"
  "        \"w\": {\n"
  "         \"lit\": 220\n"
  "        },\n"
  "        \"h\": {\n"
  "         \"lit\": 180\n"
  "        },\n"
  "        \"color\": \"darkgrey\"\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"render\": {\n"
  "        \"type\": \"graph\",\n"
  "        \"slot\": {\n"
  "         \"slot\": \"temp\"\n"
  "        }\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"add\": {\n"
  "        \"slot\": \"sample_idx\",\n"
  "        \"delta\": {\n"
  "         \"lit\": 1\n"
  "        }\n"
  "       }\n"
  "      },\n"
  "      {\n"
  "       \"if\": {\n"
  "        \"guard\": {\n"
  "         \"op\": \"lt\",\n"
  "         \"a\": {\n"
  "          \"slot\": \"sample_cnt\"\n"
  "         },\n"
  "         \"b\": {\n"
  "          \"lit\": 100\n"
  "         }\n"
  "        },\n"
  "        \"then\": [\n"
  "         {\n"
  "          \"add\": {\n"
  "           \"slot\": \"sample_cnt\",\n"
  "           \"delta\": {\n"
  "            \"lit\": 1\n"
  "           }\n"
  "          }\n"
  "         }\n"
  "        ]\n"
  "       }\n"
  "      }\n"
  "     ]\n"
  "    }\n"
  "   ]\n"
  "  }\n"
  " ]\n"
  "}\n"
;

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
   Home screen — firmware-level launcher (see globals above).
   Lists tools from the store (fallback: builtins + LittleFS
   cache), lets the user pick one, and is entered by tapping
   the persistent top-left home button on ANY screen.
   ========================================================== */
static String stripJsonExt(const String& f) {
  if (f.endsWith(".json")) return f.substring(0, f.length() - 5);
  return f;
}

// Fetch the store's tool list into homeList[]. Returns count (0 = none/unreachable).
static int loadStoreToolList() {
  int n = 0;
  if (wifiStationOk) {
    HTTPClient http;
    String url = storeUrl;
    if (!url.endsWith("/")) url += "/";
    http.begin(url.c_str()); http.setTimeout(5000);
    int code = http.GET();
    if (code == 200) {
      String payload = http.getString();
      http.end();
      JsonDocument doc;
      if (!deserializeJson(doc, payload)) {
        JsonArray arr = doc["behaviors"].as<JsonArray>();
        for (JsonVariant v : arr) {
          if (n >= HOME_LIST_MAX) break;
          String id = stripJsonExt(v.as<String>());
          if (id.length()) homeList[n++] = id;
        }
      }
    } else {
      http.end();
    }
  }
  // Always ensure the firmware built-ins are present (they always work even if
  // the store dropped them or is unreachable).
  const char* builtins[4] = {"timer", "temp-logger", "reaction-game", "tap-counter"};
  for (int i = 0; i < 4; i++) {
    bool present = false;
    for (int j = 0; j < n; j++) if (homeList[j] == builtins[i]) { present = true; break; }
    if (!present && n < HOME_LIST_MAX) homeList[n++] = builtins[i];
  }
  // Also add any behavior cached in LittleFS that isn't in the store list.
  File dir = LittleFS.open("/behaviors/");
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f && n < HOME_LIST_MAX) {
      String fn = f.name();
      // openNextFile returns full path in some versions; take basename
      int slash = fn.lastIndexOf('/');
      if (slash >= 0) fn = fn.substring(slash + 1);
      String id = stripJsonExt(fn);
      if (id.length()) {
        bool present = false;
        for (int j = 0; j < n; j++) if (homeList[j] == id) { present = true; break; }
        if (!present) homeList[n++] = id;
      }
      f.close();
      f = dir.openNextFile();
    }
    dir.close();
  }
  return n;
}

// Draw the small persistent home button glyph in the top-left corner.
void drawHomeButton() {
  // Small back-arrow (return) glyph, top-left, drawn on top of the corner.
  // No filled box — just the arrow, so it reads as a control affordance.
  uint16_t cx = HOME_BTN_X + 6;   // arrow left edge (a little inset from corner)
  uint16_t cy = HOME_BTN_Y + 6;   // arrow top
  // arrowhead + shaft, ~11px tall/wide
  tft.fillRect(cx + 5, cy + 4, 6, 3, TFT_YELLOW);          // shaft
  tft.fillTriangle(cx + 1, cy + 5, cx + 8, cy + 1, cx + 8, cy + 11, TFT_YELLOW); // head
}

// Show the home screen (title + list of tools). Scrolls when > rows fit.
static void drawHomeScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("OneBoard — pick a tool", 6, 6, 2);
  tft.drawFastHLine(0, 26, 240, TFT_DARKGREY);

  int visible = HOME_ROWS_VISIBLE;
  for (int i = 0; i < visible; i++) {
    int idx = homeScroll + i;
    if (idx >= homeCount) break;
    int y = HOME_LIST_Y + i * HOME_ROW_H;
    bool cur = (homeList[idx] == activeToolId);
    if (cur) tft.fillRect(4, y - 4, 232, HOME_ROW_H - 4, TFT_NAVY);
    tft.setTextColor(cur ? TFT_CYAN : TFT_WHITE, cur ? TFT_NAVY : TFT_BLACK);
    tft.drawString(homeList[idx], 12, y, 2);
  }

  // Scroll affordance chevrons (up / down) at bottom when there's more.
  if (homeScroll > 0) {
    tft.fillTriangle(20, 300, 30, 290, 40, 300, TFT_DARKGREY);  // up
  }
  if (homeScroll + HOME_ROWS_VISIBLE < homeCount) {
    tft.fillTriangle(200, 290, 210, 300, 220, 290, TFT_DARKGREY);  // down
  }
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("tap a tool  |  <| home", 6, 310, 1);  // font 1 small hint
}

// Handle a tap while the home screen is showing. Returns true if it navigated.
static void homeHandleTouch(int16_t tx, int16_t ty) {
  // Up / down scroll chevrons
  if (ty >= 286 && ty <= 302) {
    if (tx < 60 && homeScroll > 0) { homeScroll--; drawHomeScreen(); return; }
    if (tx > 180 && homeScroll + HOME_ROWS_VISIBLE < homeCount) { homeScroll++; drawHomeScreen(); return; }
    return;
  }
  int rel = ty - HOME_LIST_Y;
  if (rel < 0) return;
  int row = rel / HOME_ROW_H;
  int idx = homeScroll + row;
  if (row >= HOME_ROWS_VISIBLE || idx >= homeCount || idx < 0) return;
  // Load + run the picked tool.
  Serial.printf("Home: loading %s\n", homeList[idx].c_str());
  homeMode = false;
  loadBehavior(homeList[idx].c_str());
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
      ".netlist{max-width:280px;margin:0 auto;text-align:left}"
      ".net{display:flex;align-items:center;justify-content:space-between;gap:8px;"
        "padding:10px 12px;margin:6px 0;border:1px solid #444;border-radius:8px;"
        "background:#1c1c1c;cursor:pointer;font-size:0.95em}"
      ".net:hover{border-color:#4af;background:#262626}"
      ".net.picked{border-color:#4af;background:#12324a}"
      ".net .nm{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
      ".net .rssi{flex:0 0 auto;font-size:0.75em;color:#8ab}"
      ".hint{font-size:0.75em;color:#777;max-width:280px;margin:0 auto 6px}"
    "</style></head><body>"
    "<h1>&#9670; OneBoard</h1>"
    "<div class='store-url' id='storeLabel'></div>"
    "<div class='demo-name' id='current'></div>"
    "<div id='provBanner' style='display:none;border:2px solid #f80;border-radius:10px;padding:10px;margin:10px auto;max-width:280px;color:#ff0;font-size:0.95em'>"
      "&#128225; FIRST SETUP — pick your home WiFi below, then the board joins it and you can reach it there from now on."
    "</div>"
    "<div id='tools' style='text-align:left;max-width:280px;margin:10px auto'>"
      "<div style='font-size:0.9em;color:#9cf;margin-bottom:6px'>Tools (from store)</div>"
      "<div id='toolList'>loading...</div>"
    "</div>"
    "<div style='margin-top:18px;border-top:1px solid #444;padding-top:12px'>"
      "<div style='font-size:0.9em;color:#9cf;margin-bottom:6px'>WiFi (join your home network)</div>"
      "<div class='hint'>Tap a network below to fill the name. Only 2.4 GHz networks appear - that is the band the board can join. Hidden networks never show, so the text field stays for those.</div>"
      "<button onclick='scanWifi()' id='scanBtn' style='background:#333;border-color:#9cf'>Scan for networks</button>"
      "<div id='netList' class='netlist'></div>"
      "<input id='wifiSsid' type='text' style='width:100%;max-width:280px;padding:8px;background:#222;color:#fff;border:1px solid #555'"
        " placeholder='Home WiFi SSID' />"
      "<input id='wifiPass' type='password' style='width:100%;max-width:280px;padding:8px;background:#222;color:#fff;border:1px solid #555'"
        " placeholder='Home WiFi password' />"
      "<button onclick='setWifi()' style='background:#333;border-color:#9cf'>Join Home WiFi</button>"
      "<div id='wifiStatus' style='font-size:0.8em;color:#888'></div>"
    "</div>"
    "<div style='margin-top:18px;border-top:1px solid #444;padding-top:12px'>"
      "<div style='font-size:0.9em;color:#4af;margin-bottom:6px'>Store</div>"
      "<div class='hint'>The board looks for a store on your network by itself. Only touch this if you run the store somewhere it can't find.</div>"
      "<button onclick='findStore()' id='findBtn' style='background:#333;border-color:#4af'>Find store automatically</button>"
      "<div id='storeStatus' style='font-size:0.8em;color:#888;margin-bottom:8px'></div>"
      "<input id='storeInput' type='text' style='width:100%;max-width:280px;padding:8px;background:#222;color:#fff;border:1px solid #555'"
        " placeholder='Store URL (manual override)' />"
      "<button onclick='setStore()' style='background:#333'>Set Store URL manually</button>"
    "</div>"
    "<div class='footer'>OneBoard — CYD 2432S028 (self-describing)</div>"
    "<script>"
      "async function loadTool(id){"
        "try{"
          "var r=await fetch('/load?id='+id);"
          "var j=await r.json();"
          "if(j.ok)document.getElementById('current').innerText='Tool: '+j.tool;"
          "else alert('Load failed: '+(j.error||'unknown reason'));"
        "}catch(e){alert('Load failed: network error')}"
      "}"
      "async function renderTools(storeUrl){"
        "try{"
          "var r=await fetch(storeUrl.replace(/\\/$/,'')+'/');"
          "var j=await r.json();"
          "var el=document.getElementById('toolList');"
          "if(!j.behaviors||!j.behaviors.length){el.innerText='(none in store)';return}"
          "el.innerHTML='';"
          "j.behaviors.forEach(function(f){"
            "var id=f.replace(/\\.json$/,'');"
            "var b=document.createElement('button');"
            "b.id='btn-'+id;b.innerText=id;b.onclick=function(){loadTool(id)};"
            "el.appendChild(b)"
          "})"
        "}catch(e){document.getElementById('toolList').innerText='store unreachable'}"
      "}"
      "async function findStore(){"
        "var st=document.getElementById('storeStatus');"
        "st.innerText='Looking for a store on your network (a few seconds)...';"
        "try{"
          "var r=await fetch('/store?auto=1');"   // also re-enables auto discovery
          "var j=await r.json();"
          "if(j.found){"
            "st.innerText='Found: '+j.url;"
            "document.getElementById('storeLabel').innerText='Store: '+j.url;"
            "document.getElementById('storeInput').value=j.url;"
            "renderTools(j.url)"
          "}else{"
            "st.innerText='No store found. Start store/serve.py, then try again - or set the URL below.'"
          "}"
        "}catch(e){st.innerText='Discovery failed - set the URL below.'}"
      "}"
      "async function setStore(){"
        "var v=document.getElementById('storeInput').value.trim();"
        "if(!v)return;"
        "try{"
          "var r=await fetch('/store?url='+encodeURIComponent(v));"
          "var j=await r.json();"
          "if(j.ok){"
            "document.getElementById('storeLabel').innerText='Store: '+j.url;"
            "document.getElementById('storeStatus').innerText="
              "'Manual URL set - automatic discovery is now off. Use Find store automatically to go back.'"
          "}"
        "}catch(e){alert('Failed')}"
      "}"
      "var scanTimer=null;"
      "function pickNet(ssid,el){"
        "document.getElementById('wifiSsid').value=ssid;"
        "var all=document.querySelectorAll('.net');"
        "for(var i=0;i<all.length;i++)all[i].className='net';"
        "if(el)el.className='net picked';"
      "}"
      "function renderNets(list){"
        "var el=document.getElementById('netList');"
        "el.innerHTML='';"  // clearing only - every network name goes in via innerText
        "if(!list.length){el.innerText='No networks found - tap Scan again.';return}"
        "list.forEach(function(n){"
          "var d=document.createElement('div');d.className='net';"
          "var a=document.createElement('span');a.className='nm';"
          "a.innerText=n.ssid+(n.secure?'':' (open)');"
          "var b=document.createElement('span');b.className='rssi';b.innerText=n.rssi+' dBm';"
          "d.appendChild(a);d.appendChild(b);"
          "d.onclick=function(){pickNet(n.ssid,d)};"
          "el.appendChild(d)"
        "})"
      "}"
      "function pollScan(){"
        "fetch('/scan').then(function(r){return r.json()}).then(function(j){"
          "if(j.state==='running'){scanTimer=setTimeout(pollScan,700);return}"
          "scanTimer=null;"
          "document.getElementById('scanBtn').innerText='Scan for networks';"
          "renderNets(j.networks||[])"
        "}).catch(function(){scanTimer=null;"
          "document.getElementById('scanBtn').innerText='Scan for networks'})"
      "}"
      "function scanWifi(){"
        "if(scanTimer)return;"
        "var st=document.getElementById('wifiStatus');"
        "st.innerText='Scanning - the access point may stutter for a second.';"
        "document.getElementById('scanBtn').innerText='Scanning...';"
        "fetch('/scan?start=1').then(function(){scanTimer=setTimeout(pollScan,800)})"
          ".catch(function(){scanTimer=null;st.innerText='Scan failed'})"
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
        "document.getElementById('storeStatus').innerText="
          "j.store_found?'Found automatically - nothing to do.'"
          ":(j.store_auto?'No store answered. Start store/serve.py and tap Find store automatically.'"
          ":'Automatic discovery is off (manual URL).');"
        "if(j.provisioning){document.getElementById('provBanner').style.display='block';scanWifi()}"
        "renderTools(j.store_url);"
        "var b=document.getElementById('btn-'+j.tool);"
        "if(b)b.className='active'"
      "}).catch(()=>{})"
    "</script></body></html>");
  // Never let a browser serve a stale copy of the setup page: a cached page
  // shows a form whose state (provisioning? store URL? tool list?) is wrong.
  server.sendHeader("Cache-Control", "no-store, must-revalidate");
  server.send(200, "text/html", html);
}

void handleLoad() {
  if (server.hasArg("id")) {
    String id = server.arg("id");
    bool ok = loadBehavior(id.c_str());
    if (ok) {
      // Optional home control so a script can drive a demo without a finger:
      //   /load?id=X&home=0  -> leave the launcher and show the loaded tool
      //   /load?id=X&home=1  -> (re)open the launcher, tool paused
      // Omitting the arg leaves the current mode alone, so existing clients
      // (the web page, the Telegram bot) behave exactly as before.
      if (server.hasArg("home")) {
        bool wantHome = server.arg("home") != "0";
        if (wantHome && !homeMode) {
          homeScroll = 0;
          homeCount = loadStoreToolList();
          homeMode = true;
          drawHomeScreen();
        } else if (!wantHome && homeMode) {
          homeMode = false;
          if (vmInitialized) { vmRender(&g_state, &g_prog, &tft); drawHomeButton(); }
        }
      }
      String json = "{\"ok\":true,\"tool\":\"" + id + "\",\"home\":";
      json += homeMode ? "true" : "false";
      json += "}";
      server.send(200, "application/json", json);
    } else {
      // Report the real reason (compile error, no source, bad JSON) so a
      // broken tool doesn't look loaded.
      String msg = lastLoadError;
      msg.replace("\\", "\\\\"); msg.replace("\"", "\\\"");
      String json = "{\"ok\":false,\"tool\":\"" + id + "\",\"error\":\"" + msg + "\"}";
      server.send(200, "application/json", json);
    }
    return;
  }
  server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}");
}

void handleTouchDebug() {
  String json = "{\"raw\":[";
  json += lastRawX;
  json += ",";
  json += lastRawY;
  json += "],\"z\":";
  json += lastRawZ;
  json += ",\"taps\":[";
  for (int i = 0; i < tapBufN; i++) {
    if (i) json += ",";
    json += "[";
    json += tapBufX[i]; json += ","; json += tapBufY[i];
    json += "]";
  }
  json += "],\"count\":";
  json += tapBufN;
  json += "}";
  server.send(200, "application/json", json);
}

void handleTouchMap() {
  // Report where the last press maps in calibrated screen coords — the live
  // proof the mapping is right (raw bottom-right → ~[239,319], top-left → [0,0]).
  uint16_t sx = 999, sy = 999;
  if (lastRawX >= 0 && lastRawY >= 0) {
    int32_t rx = ((int32_t)lastRawX - TX_MIN) * 240 / (TX_MAX - TX_MIN);
    int32_t ry = ((int32_t)lastRawY - TY_MIN) * 320 / (TY_MAX - TY_MIN);
    if (rx < 0) rx = 0; else if (rx > 239) rx = 239;
    if (ry < 0) ry = 0; else if (ry > 319) ry = 319;
    sx = (uint16_t)rx; sy = (uint16_t)ry;
  }
  String json = "{\"screen\":[";
  json += sx; json += ","; json += sy;
  json += "],\"raw\":[";
  json += lastRawX; json += ","; json += lastRawY;
  json += "]}";
  server.send(200, "application/json", json);
}

/* ==========================================================
   WiFi scan — provisioning helper.

   Lets the setup page offer the real networks instead of making you type an
   SSID. Three things shape how this works:

   * It is ASYNC on purpose. The ESP32 has one radio; a scan hops all 13
     channels, so while it runs the soft AP is off its channel and a connected
     phone can stutter or briefly drop. A blocking scan would hold that for 2-4s
     inside the HTTP request, so instead we start the scan, answer immediately,
     and the page polls /scan until it reports done.
   * Only 2.4 GHz networks appear. The ESP32 has no 5 GHz radio and could not
     join a 5 GHz network anyway, so the list is honest by construction.
   * Hidden networks carry no SSID at all, so they are skipped - there is
     nothing to display. That is why the form keeps its free-text field.
   ========================================================== */
#define SCAN_MAX_SHOWN 12

void handleScan() {
  if (server.hasArg("start")) {
    // async=true: enables the STA interface itself and returns immediately.
    WiFi.scanNetworks(true, false);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"state\":\"running\"}");
    return;
  }

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"state\":\"running\"}");
    return;
  }
  if (n < 0) {            // failed, or no scan has been started yet
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"state\":\"idle\",\"networks\":[]}");
    return;
  }

  String  names[SCAN_MAX_SHOWN];
  int32_t sig[SCAN_MAX_SHOWN];
  bool    secure[SCAN_MAX_SHOWN];
  int     found = 0;

  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;                 // hidden: nothing to pick
    int32_t r = WiFi.RSSI(i);
    bool sec = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

    // One SSID often has several APs behind it (mesh, or the same name on both
    // bands), so dedupe by name and keep the strongest signal.
    int at = -1;
    for (int j = 0; j < found; j++) if (names[j] == s) { at = j; break; }
    if (at < 0) {
      if (found >= SCAN_MAX_SHOWN) continue;       // list full; don't grow
      at = found++;
      names[at] = s; sig[at] = r; secure[at] = sec;
    } else if (r > sig[at]) {
      sig[at] = r; secure[at] = sec;
    }
  }

  // Strongest first — the order that helps pick the AP you're actually near.
  for (int i = 1; i < found; i++) {
    String  kn = names[i]; int32_t kr = sig[i]; bool ks = secure[i];
    int j = i - 1;
    while (j >= 0 && sig[j] < kr) {
      names[j + 1] = names[j]; sig[j + 1] = sig[j]; secure[j + 1] = secure[j];
      j--;
    }
    names[j + 1] = kn; sig[j + 1] = kr; secure[j + 1] = ks;
  }

  String json = "{\"state\":\"done\",\"networks\":[";
  for (int i = 0; i < found; i++) {
    if (i) json += ",";
    String esc = names[i];
    esc.replace("\\", "\\\\");                     // SSIDs are arbitrary bytes
    esc.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + esc + "\",\"rssi\":" + String(sig[i]) +
            ",\"secure\":" + (secure[i] ? "true" : "false") + "}";
  }
  json += "]}";

  WiFi.scanDelete();                               // free the result set
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleWifi() {
  if (server.hasArg("ssid") && server.hasArg("pass")) {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    saveConfigStr(WIFI_SSID_KEY, ssid.c_str());
    saveConfigStr(WIFI_PASS_KEY, pass.c_str());
    bool ok = wifiStationConnect(false);
    if (ok) provisioningMode = false;  // done setting up
    // We're on the network now — go find the store before answering, so the page
    // that reloads after this sees a working one. Costs up to ~3s if nothing
    // answers, which is why the page says "Joining...".
    if (ok) applyStoreDiscovery();
    String ip = ok ? WiFi.localIP().toString() : "";
    String json = "{\"ok\":";
    json += ok ? "true" : "false";
    json += ",\"ssid\":\"" + ssid + "\"";
    json += ",\"ip\":\"" + ip + "\"";
    json += ",\"store\":\"" + storeUrl + "\"";
    json += ",\"store_found\":";
    json += storeFound ? "true" : "false";
    json += "}";
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
  json += ",\"provisioning\":";
  json += provisioningMode ? "true" : "false";
  json += ",\"ip\":\"";
  json += wifiStationOk ? WiFi.localIP().toString() : "";
  json += "\",\"home\":";
  json += homeMode ? "true" : "false";
  json += ",\"store_auto\":";
  json += storeAuto ? "true" : "false";
  json += ",\"store_found\":";
  json += storeFound ? "true" : "false";
  json += "}";
  server.send(200, "application/json", json);
}

/* ==========================================================
   /state — live VM state: every slot (name, type, value) plus
   the render primitives. Any client can use this (with /schema
   and the behavior JSON from the store) to render the board's
   screen itself — the "any-client self-description" story.
   ========================================================== */
void handleState() {
  String json = "{\"tool\":\"";
  json += activeToolId;
  json += "\",\"slots\":[";
  for (uint8_t i = 0; i < g_prog.slot_count; i++) {
    if (i) json += ",";
    json += "{\"name\":\"";
    json += g_prog.slots[i].name;
    json += "\",\"type\":\"";
    switch (g_prog.slots[i].type) {
      case SLOT_INT64: json += "int64"; break;
      case SLOT_FLOAT: json += "float"; break;
      case SLOT_BOOL:  json += "bool";  break;
      case SLOT_STRING: json += "string"; break;
    }
    json += "\",\"value\":";
    switch (g_prog.slots[i].type) {
      case SLOT_STRING:
        json += "\"";
        json += g_state.sval[i];
        json += "\"";
        break;
      case SLOT_FLOAT: {
        char b[16];
        dtostrf(g_state.fval[i], 0, 1, b);
        json += b;
        break;
      }
      default:
        json += String((long long)g_state.ival[i]);
        break;
    }
    json += "}";
  }
  json += "],\"renders\":[";
  for (uint8_t i = 0; i < g_prog.render_count; i++) {
    if (i) json += ",";
    json += "{\"type\":";
    json += g_prog.renders[i].type;
    json += "}";
  }
  json += "]}";
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
    "\"verbs\":[\"add\",\"sub\",\"mul\",\"div\",\"mod\",\"clamp\",\"set\",\"setf\",\"toggle\",\"stamp\",\"random\",\"now_minus\",\"format\",\"progress\",\"render\",\"if\"],"
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
      "\"touch regions: event-region {x,y,w,h} hit-test, no region = full screen\","
      "\"renders draw at frame flush (mutate state before render verbs)\","
      "\"compile errors are fatal and reported by /load\","
      "\"see docs/BEHAVIOR-AUTHORING.md + tools/validate_behavior.py\"]"
    "}";
  server.send(200, "application/json", schema);
}

void handleDelete() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing id\"}");
    return;
  }
  String id = server.arg("id");
  // Only purge the board's LittleFS CACHE copy. The authoritative removal is
  // the store (store/serve.py DELETE); this endpoint just makes sure /load
  // can't resurrect a deleted tool from the board's local cache.
  String path = String(BEHAVIOR_DIR) + id + ".json";
  bool had = LittleFS.exists(path.c_str());
  if (had) LittleFS.remove(path.c_str());
  // If the deleted tool is what's currently on screen, fall back to the default
  // built-in so the panel isn't stuck showing a tool that no longer exists in
  // the store.
  if (activeToolId == id) {
    activeToolId = DEFAULT_TOOL;
    saveConfigStr(ACTIVE_KEY, DEFAULT_TOOL);
    loadBehavior(DEFAULT_TOOL);
  }
  String json = "{\"ok\":true,\"cached\":";
  json += had ? "true" : "false";
  json += ",\"id\":\"" + id + "\"}";
  server.send(200, "application/json", json);
}

void handleStore() {
  // /store?auto=1  -> re-enable discovery and go looking for a store right now
  // /store?auto=0  -> stop discovery from replacing the configured URL
  if (server.hasArg("auto")) {
    storeAuto = (server.arg("auto") != "0");
    saveConfigStr(STORE_AUTO_KEY, storeAuto ? "1" : "0");
    bool found = storeAuto ? applyStoreDiscovery() : false;
    String json = "{\"ok\":true,\"auto\":";
    json += storeAuto ? "true" : "false";
    json += ",\"found\":";
    json += found ? "true" : "false";
    json += ",\"url\":\"" + storeUrl + "\"}";
    server.send(200, "application/json", json);
    return;
  }
  if (server.hasArg("url")) {
    storeUrl = server.arg("url");
    saveConfigStr(STORE_KEY, storeUrl.c_str());
    // An address typed in by hand is a deliberate choice, so it wins: turn
    // discovery off rather than let the next boot silently replace it.
    storeAuto  = false;
    storeFound = false;
    saveConfigStr(STORE_AUTO_KEY, "0");
    String json = "{\"ok\":true,\"url\":\"" + storeUrl + "\",\"auto\":false}";
    server.send(200, "application/json", json);
    return;
  }
  server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
}

/* /discover — browse for the store on demand (the setup page's button).
   Reads the same records the board uses at boot.~3s worst case. */
void handleDiscover() {
  bool found = applyStoreDiscovery();
  String json = "{\"found\":";
  json += found ? "true" : "false";
  json += ",\"auto\":";
  json += storeAuto ? "true" : "false";
  json += ",\"url\":\"" + storeUrl + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleNotFound() {
  // Captive-portal detection: phones probe well-known URLs when joining a
  // WiFi network. Answer them with a redirect to the config page so iOS /
  // Android auto-open the setup form (the whole point of "join my AP and it
  // just works"). Also redirect anything aimed at the AP during provisioning.
  String host = server.hostHeader();
  String path = server.uri();
  bool isProbe =
      path == "/generate_204" || path == "/gen_204" ||
      path == "/hotspot-detect.html" || path == "/canonical.html" ||
      path == "/success.txt" || path == "/ncsi.txt" ||
      path == "/connecttest.txt" || path == "/library/test/success.html" ||
      path == "/check_network_status.txt" ||
      path == "/fwlink/" || host.indexOf("connectivitycheck") >= 0 ||
      host.indexOf("gstatic.com") >= 0 || host.indexOf("apple.com") >= 0 ||
      host.indexOf("msftconnecttest") >= 0;
  if (isProbe || provisioningMode) {
    // 302 to the config page — the phone's captive browser follows it and
    // shows our setup form automatically.
    server.sendHeader("Location", "http://192.168.4.1/", true);
    // no-store so the phone re-asks us instead of replaying a cached redirect
    server.sendHeader("Cache-Control", "no-store, must-revalidate");
    server.send(302, "text/plain", "redirect");
    return;
  }
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
  storeAuto = loadConfigStr(STORE_AUTO_KEY, "1") != "0";
  activeToolId = loadConfigStr(ACTIVE_KEY, DEFAULT_TOOL);

  // Provisioning mode: no saved WiFi creds -> keep the captive portal as the
  // primary experience (redirect everything to the setup form).
  provisioningMode = (loadConfigStr(WIFI_SSID_KEY, "").length() == 0);

  tryStationConnect();

  // Find the store on our own instead of waiting to be told where it is. Costs
  // up to ~3s of boot time when nothing answers, and is a no-op if discovery is
  // off or we have no network. Nothing is persisted, so a store host whose
  // address changed is picked up again on the next boot.
  applyStoreDiscovery();

  WiFi.softAP(SOFT_AP_SSID, SOFT_AP_PSK);

  // Captive-portal DNS: answer EVERY domain with the AP's IP so a phone that
  // joins the board's WiFi resolves any hostname to us and the captive portal
  // opens. Only needed on the AP (always up).
  dnsServer.start(53, "*", WiFi.softAPIP());
  if (provisioningMode) {
    Serial.println("PROVISIONING MODE — join AP to set WiFi");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Join AP to set up:", 10, 60, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String(SOFT_AP_SSID), 10, 90, 2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Open browser, page opens", 10, 140, 2);
    tft.drawString("http://192.168.4.1", 10, 170, 2);
  }

  seedDefaultBehaviors();
  loadBehavior(activeToolId.c_str());

  server.on("/",        handleRoot);
  server.on("/load",    handleLoad);
  server.on("/status",  handleStatus);
  server.on("/state",   handleState);
  server.on("/schema",  handleSchema);
  server.on("/store",   handleStore);
  server.on("/delete",  handleDelete);
  server.on("/touch",   handleTouchDebug);
  server.on("/touchmap", handleTouchMap);
  server.on("/scan",    handleScan);
  server.on("/discover", handleDiscover);
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
  dnsServer.processNextRequest();
  unsigned long now = millis();

  // Home mode: the tool is paused (not ticking); the screen shows the launcher.
  if (vmInitialized && !homeMode) {
    // Run scheduled events (ticks); redraw only when something actually ran
    // (Stage 2 behavior — a tick or touch re-renders; no idle redraw storm).
    bool ran = vmTick(&g_state, &g_prog, now);
    if (ran) { vmRender(&g_state, &g_prog, &tft); drawHomeButton(); }
  }

  /* Touch → touch events (edge-detected, 250ms cooldown).
     The top-left home region is intercepted FIRST — before the running tool sees
     it — so a full-screen tool can never hijack the home button. Tapping it
     opens the home screen (tool list). Then vmDispatchTouch feeds remaining
     presses to the active tool, or the home screen handles row selection. */
  static bool touchWasDown = false;
  static uint32_t lastTouchEventMs = 0;
  uint16_t tx = 0, ty = 0;
  bool touched = touchReadScreen(&tx, &ty);
  uint32_t tNow = millis();
  if (touched && !touchWasDown && (tNow - lastTouchEventMs > 250)) {
    lastTouchEventMs = tNow;

    bool inHomeBtn = (tx >= HOME_BTN_X && tx < HOME_BTN_X + HOME_BTN_W &&
                      ty >= HOME_BTN_Y && ty < HOME_BTN_Y + HOME_BTN_H);

    if (homeMode) {
      // We're already on the home screen — route the tap to it (row select /
      // scroll). Tapping the home-button area again is harmless here.
      homeHandleTouch((int16_t)tx, (int16_t)ty);
    } else if (inHomeBtn) {
      // User tapped the persistent home button → open the tool launcher.
      homeScroll = 0;
      homeCount = loadStoreToolList();
      homeMode = true;
      drawHomeScreen();
    } else if (vmInitialized) {
      if (vmDispatchTouch(&g_state, &g_prog, (int16_t)tx, (int16_t)ty)) {
        vmRender(&g_state, &g_prog, &tft);
        drawHomeButton();   // tool re-rendered — keep home button on top
      }
    }
  }
  touchWasDown = touched;

  delay(LOOP_DELAY);
}