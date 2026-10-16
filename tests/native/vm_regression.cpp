#include "vm.h"
#include <cassert>
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
int main(int argc,char** argv) {
  limits(); renderLimit(); references(); progress(); mathVerbs(); initialBranches();
  for(int i=1;i<argc;++i) {
    std::ifstream input(argv[i]); JsonDocument doc; assert(!deserializeJson(doc,input));
    VmProgram p; if(!vmCompile(doc.as<JsonObject>(),&p)) { std::cerr<<argv[i]<<": "<<p.compile_error<<"\n"; return 1; }
    VmState s; vmStateInit(&s,&p); vmMarkInitialRender(&s,&p);
  }
  std::cout<<"VM regression tests passed\n";
}
