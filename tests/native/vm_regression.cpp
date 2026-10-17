#include "vm.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

static JsonDocument makeTool() {
  JsonDocument doc;
  deserializeJson(doc, R"({"id":"test","state":[{"name":"n","type":"int64","init":50},{"name":"w","type":"int64","init":0}],"events":[{"name":"touch-down","handlers":[{"run":[]}]}]})");
  return doc;
}
static JsonArray run(JsonDocument& doc) { return doc["events"][0]["handlers"][0]["run"].as<JsonArray>(); }
static void rejects(JsonDocument& doc, const char* reason) {
  VmProgram program;
  assert(!vmCompile(doc.as<JsonObject>(), &program));
  assert(strstr(program.compile_error, reason));
}
static void limits() {
  { auto d = makeTool(); auto slots = d["state"].as<JsonArray>();
    for (int i=2; i<=VM_MAX_SLOTS; ++i) { auto s=slots.add<JsonObject>(); s["name"]=std::string("s")+std::to_string(i); s["type"]="int64"; }
    rejects(d,"slot limit"); }
  { auto d=makeTool(); auto events=d["events"].as<JsonArray>();
    for(int i=1; i<=VM_MAX_EVENTS; ++i) { auto e=events.add<JsonObject>(); e["name"]="extra"; }
    rejects(d,"event limit"); }
  { auto d=makeTool(); auto handlers=d["events"][0]["handlers"].as<JsonArray>();
    for(int i=1; i<=VM_MAX_HANDLERS; ++i) handlers.add<JsonObject>();
    rejects(d,"handler limit"); }
  { auto d=makeTool(); auto events=d["events"].as<JsonArray>(); events.clear();
    for(int i=0; i<3; ++i) { auto e=events.add<JsonObject>(); e["name"]="event";
      auto hs=e["handlers"].to<JsonArray>(); for(int j=0;j<5;++j) hs.add<JsonObject>(); }
    rejects(d,"guard limit"); }
  { auto d=makeTool(); auto block=run(d);
    for(int i=0;i<VM_MAX_INSTRUCTIONS;++i) block.add<JsonObject>()["toggle"]["slot"]="n";
    VmProgram p; assert(vmCompile(d.as<JsonObject>(),&p)); assert(p.instr_count==VM_MAX_INSTRUCTIONS);
    block.add<JsonObject>()["toggle"]["slot"]="n"; rejects(d,"instruction limit"); }
}
static void renderLimit() {
  auto d = makeTool();
  auto block = run(d);
  for (int i = 0; i < VM_MAX_RENDER; ++i)
    block.add<JsonObject>()["render"]["type"] = "clear";
  VmProgram p;
  assert(vmCompile(d.as<JsonObject>(), &p));
  block.add<JsonObject>()["render"]["type"] = "clear";
  rejects(d, "render limit");
}
static void references() {
  for(const char* key : {"delta","value","from"}) {
    auto d=makeTool(); const char* verb=!strcmp(key,"delta")?"add":!strcmp(key,"value")?"set":"now_minus";
    auto args=run(d).add<JsonObject>()[verb].to<JsonObject>(); args["slot"]="n"; args[key]["slot"]="missing";
    rejects(d,"unknown slot");
  }
  { auto d=makeTool(); d["events"][0]["handlers"][0]["guard"]["op"]="is_true";
    d["events"][0]["handlers"][0]["guard"]["a"]["slot"]="missing"; rejects(d,"unknown slot"); }
  for(const char* key : {"x","src","color"}) {
    auto d=makeTool(); auto r=run(d).add<JsonObject>()["render"].to<JsonObject>(); r["type"]="text"; r[key]["slot"]="missing";
    rejects(d,"unknown slot");
  }
  { auto d=makeTool(); d["state"][1]["name"]="n"; rejects(d,"duplicate slot"); }
  { auto d=makeTool(); d["state"][0]["type"]="imaginary"; rejects(d,"unknown slot type"); }
  { auto d=makeTool(); d["state"][0].remove("name"); rejects(d,"name and type"); }
  { auto d=makeTool(); run(d).add<JsonObject>(); rejects(d,"exactly one verb"); }
}
static void progress() {
  for(int mode=0;mode<3;++mode) {
    auto d=makeTool(); auto args=run(d).add<JsonObject>()["progress"].to<JsonObject>();
    args["slot"]="w"; args["src"]["slot"]="n";
    if(mode) args["max"]["lit"]=100;
    if(mode==2) args["px"]["lit"]=200;
    VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,1)==(mode==0?0:mode==1?110:100));
    assert(p.instrs[0].b_val==(mode?100:1)); assert(p.instrs[0].c_val==(mode==2?200:220));
  }
}
static void exec1(JsonDocument& d, const char* verb, const char* slot, int64_t value) {
  auto args=run(d).add<JsonObject>()[verb].to<JsonObject>(); args["slot"]=slot; args["value"]["lit"]=value;
}
static void mathVerbs() {
  // mul: n(50) * 3 = 150
  { auto d=makeTool(); exec1(d,"mul","n",3); VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,0)==150); }
  // mul on float slot preserves float type
  { auto d=makeTool(); d["state"][0]["type"]="float"; d["state"][0]["init"]=2.5f; exec1(d,"mul","n",2);
    VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetFloat(&s,0)>4.99f && vmGetFloat(&s,0)<5.01f); }
  // div: 50 / 4 = 12
  { auto d=makeTool(); exec1(d,"div","n",4); VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,0)==12); }
  // div by zero guards -> 0 (no crash)
  { auto d=makeTool(); exec1(d,"div","n",0); VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,0)==0); }
  // mod: 50 % 8 = 2
  { auto d=makeTool(); exec1(d,"mod","n",8); VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,0)==2); }
  // mod by zero guards -> 0
  { auto d=makeTool(); exec1(d,"mod","n",0); VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,0)==0); }
  // clamp: init n=50, clamp [10,20] -> 20 (above hi)
  { auto d=makeTool(); auto args=run(d).add<JsonObject>()["clamp"].to<JsonObject>(); args["slot"]="n"; args["min"]["lit"]=10; args["max"]["lit"]=20;
    VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,0)==20); }
  // clamp below hi: init n=5, clamp [10,20] -> 10
  { auto d=makeTool(); d["state"][0]["init"]=5; auto args=run(d).add<JsonObject>()["clamp"].to<JsonObject>(); args["slot"]="n"; args["min"]["lit"]=10; args["max"]["lit"]=20;
    VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,0)==10); }
  // clamp inside: init n=15, [10,20] -> 15 unchanged
  { auto d=makeTool(); d["state"][0]["init"]=15; auto args=run(d).add<JsonObject>()["clamp"].to<JsonObject>(); args["slot"]="n"; args["min"]["lit"]=10; args["max"]["lit"]=20;
    VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,0)==15); }
  // clamp reversed bounds still works (min>max -> swap)
  { auto d=makeTool(); d["state"][0]["init"]=5; auto args=run(d).add<JsonObject>()["clamp"].to<JsonObject>(); args["slot"]="n"; args["min"]["lit"]=200; args["max"]["lit"]=10;
    VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    assert(vmDispatch(&s,&p,"touch-down")); assert(vmGetInt(&s,0)==10); }
}
static void initialBranches() {
  for(int value : {0,1}) {
    auto d=makeTool(); d["state"][0]["init"]=value;
    auto branch=run(d).add<JsonObject>()["if"].to<JsonObject>();
    branch["guard"]["op"]="is_true"; branch["guard"]["a"]["slot"]="n";
    branch["then"].to<JsonArray>().add<JsonObject>()["render"]["type"]="clear";
    auto nested=branch["else"].to<JsonArray>().add<JsonObject>()["if"].to<JsonObject>();
    nested["guard"]["op"]="always";
    nested["then"].to<JsonArray>().add<JsonObject>()["render"]["type"]="clear";
    nested["else"].to<JsonArray>().add<JsonObject>()["render"]["type"]="clear";
    run(d).add<JsonObject>()["add"]["slot"]="n";
    VmProgram p; VmState s; assert(vmCompile(d.as<JsonObject>(),&p)); vmStateInit(&s,&p);
    vmMarkInitialRender(&s,&p); assert(s.render_dirty==(value?1u:2u)); assert(vmGetInt(&s,0)==value);
  }
}
// Self-erasing text. A value that updates every tick used to need a full-screen
// 'clear' to erase stale digits — 150 KB over the CYD's 20 MHz SPI, ~61 ms, so a
// 100 ms tick left the panel black 61% of the time and visibly flickered. Text
// with "bg" blanks only its own box instead. Assert both halves: the box is
// painted (and correctly placed), and the screen is NOT cleared.
static void selfErasingText() {
  TFT_eSPI tft;
  auto build = [](bool withBg, bool withWh, bool withClear) {
    // Build the tool as JSON text: growing an ArduinoJson document's pool
    // invalidates references to it, so hand-assembling nodes loses writes.
    std::string r = R"({"render":{"type":"text","x":{"lit":120},"y":{"lit":120},)"
                    R"("src":{"slot":"display"},"font":{"lit":6},"color":"white","center":true)";
    if (withBg) {
      r += R"(,"bg":"black")";
      if (withWh) r += R"(,"w":{"lit":200},"h":{"lit":56})";
    }
    r += "}}";
    std::string renders = withClear ? R"({"render":{"type":"clear","color":"black"}},)" : "";
    renders += r;
    // slot 0 is deliberately 300 — larger than the text box — so if the compiler
    // ever regresses to resolving an absent w/h from slot 0, the geometry
    // assertions below fail loudly instead of quietly clamping.
    std::string json = R"({"id":"t","state":[{"name":"n","type":"int64","init":300},)"
                       R"({"name":"display","type":"string","init":"00:01"}],)"
                       R"("events":[{"name":"tick","schedule":{"type":"interval","ms":100},)"
                       R"("handlers":[{"run":[)" + renders + R"(]}]}]})";
    JsonDocument d;
    assert(!deserializeJson(d, json));
    return d;
  };
  auto draw = [&](JsonDocument& d, int expect_screen_clears) {
    VmProgram p; VmState s;
    assert(vmCompile(d.as<JsonObject>(), &p));
    vmStateInit(&s, &p);
    vmMarkInitialRender(&s, &p);
    TFT_eSPI::resetCalls();
    vmRender(&s, &p, &tft);
    // vmRender itself only clears when the behaviour asks for it; the single
    // load-time clear for a tool switch is main.cpp's job, not the VM's.
    assert(TFT_eSPI::fillScreen_calls == expect_screen_clears);
  };
  // bg + explicit w/h: one fillRect, box centred on the datum (120,120) -> 20,92
  { auto d = build(true, true, false); draw(d, 0);
    assert(TFT_eSPI::fillRect_calls == 1);
    assert(TFT_eSPI::last_fr_x == 20 && TFT_eSPI::last_fr_y == 92 &&
           TFT_eSPI::last_fr_w == 200 && TFT_eSPI::last_fr_h == 56); }
  // bg, no w/h: box measured from the glyphs — "00:01" at font 6 = 5 x 32 x 48
  { auto d = build(true, false, false); draw(d, 0);
    assert(TFT_eSPI::fillRect_calls == 1);
    assert(TFT_eSPI::last_fr_x == 40 && TFT_eSPI::last_fr_y == 96 &&
           TFT_eSPI::last_fr_w == 160 && TFT_eSPI::last_fr_h == 48); }
  // no bg: transparent text, nothing erased (over a button, that is the point)
  { auto d = build(false, false, false); draw(d, 0); assert(TFT_eSPI::fillRect_calls == 0); }
  // a clear is still a full-screen fill, so the guard has something real to warn about
  { auto d = build(false, false, true); draw(d, 1); }
}

// Long text literals. RenderOp.text is a fixed char[VM_STR_LEN] and the compile
// step copies into it with strncpy(dst, src, VM_STR_LEN - 1) — anything longer is
// TRUNCATED, with no error and no warning. On hardware that reads as a misspelled
// word on the panel: the timer's "Touch to pause/resume" (21 chars) drew as
// "Touch to pause/resu" against a 20-byte buffer, on a 240 px screen where the
// whole string is only 139 px wide and fits with 100 px to spare. Nothing in the
// behaviour JSON was wrong — the buffer was too small.
static void longLiteralText() {
  TFT_eSPI tft;
  auto draw = [&](const char* lit) {
    // Assemble as JSON text: the literal is inserted verbatim (these test
    // strings contain no quotes or backslashes).
    std::string json = R"({"id":"t","state":[{"name":"n","type":"int64","init":0}],)"
                       R"("events":[{"name":"tick","schedule":{"type":"interval","ms":250},)"
                       R"("handlers":[{"run":[{"render":{"type":"text","x":{"lit":20},)"
                       R"("y":{"lit":210},"src":{"lit":")" + std::string(lit) +
                       R"("},"font":{"lit":2},"color":"white"}}]}]}]})";
    JsonDocument d;
    assert(!deserializeJson(d, json));
    VmProgram p; VmState s;
    assert(vmCompile(d.as<JsonObject>(), &p));
    vmStateInit(&s, &p);
    vmMarkInitialRender(&s, &p);
    TFT_eSPI::resetCalls();
    vmRender(&s, &p, &tft);
    assert(TFT_eSPI::text_calls == 1);
    return std::string(TFT_eSPI::last_text);
  };
  // The real one from the panel: 21 chars, and it fits the screen easily.
  assert(draw("Touch to pause/resume") == "Touch to pause/resume");
  assert(draw("Provisioning: 192.168.4.1") == "Provisioning: 192.168.4.1");
  assert(draw("Tap to pause or resume") == "Tap to pause or resume");
  // Boundary is derived from VM_STR_LEN, not hardcoded: a literal of exactly
  // VM_STR_LEN-1 chars survives whole.
  std::string atLimit(VM_STR_LEN - 1, 'A');
  assert(draw(atLimit.c_str()) == atLimit);
  // ...and one char longer still truncates, by design. That is why
  // tools/validate_behavior.py refuses a literal over the limit: the VM can only
  // shorten it silently, and a shorter word on the panel looks like a typo.
  std::string overLimit(VM_STR_LEN + 4, 'B');
  std::string got = draw(overLimit.c_str());
  assert(got.size() == VM_STR_LEN - 1);
  assert(got == overLimit.substr(0, VM_STR_LEN - 1));
}

// A zero-width bar must draw NOTHING. R_BAR used to stroke its outline
// unconditionally, so a bar fed from `progress` at 0 px left a 1x10 px line of
// the bar's colour sitting at its origin — on the timer's panel that was
// reported as a dead pixel. Zero width now means zero ink; an author who wants a
// visible track to fill draws a fixed-width bar in a dim colour instead.
static void zeroWidthBar() {
  TFT_eSPI tft;
  auto drawBar = [&](int wpx) {
    std::string json = R"({"id":"t","state":[{"name":"n","type":"int64","init":0}],)"
      R"("events":[{"name":"tick","schedule":{"type":"interval","ms":250},"handlers":[{"run":[)"
      R"({"render":{"type":"bar","x":{"lit":10},"y":{"lit":170},"w":{"lit":)" +
      std::to_string(wpx) + R"(},"h":{"lit":10},"color":"blue"}}]}]}]})";
    JsonDocument d;
    assert(!deserializeJson(d, json));
    VmProgram p; VmState s;
    assert(vmCompile(d.as<JsonObject>(), &p));
    vmStateInit(&s, &p);
    vmMarkInitialRender(&s, &p);
    TFT_eSPI::resetCalls();
    vmRender(&s, &p, &tft);
  };
  drawBar(0);
  assert(TFT_eSPI::fillRect_calls == 0);
  assert(TFT_eSPI::drawRect_calls == 0);
  drawBar(50);
  assert(TFT_eSPI::fillRect_calls == 1);
  assert(TFT_eSPI::drawRect_calls == 1);
  assert(TFT_eSPI::last_fr_w == 50);
}

int main(int argc,char** argv) {
  limits(); renderLimit(); references(); progress(); mathVerbs(); initialBranches();
  selfErasingText(); longLiteralText(); zeroWidthBar();
  for(int i=1;i<argc;++i) {
    std::ifstream input(argv[i]); JsonDocument doc; assert(!deserializeJson(doc,input));
    VmProgram p; if(!vmCompile(doc.as<JsonObject>(),&p)) { std::cerr<<argv[i]<<": "<<p.compile_error<<"\n"; return 1; }
    VmState s; vmStateInit(&s,&p); vmMarkInitialRender(&s,&p);
  }
  std::cout<<"VM regression tests passed\n";
}
