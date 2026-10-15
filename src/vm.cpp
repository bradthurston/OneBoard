/* ==========================================================
   OneBoard Stage 3 — VM compiler + executor
   ========================================================== */
#include "vm.h"

// ============================================================
// Colour map (matches resolveColour in original main.cpp)
// ============================================================
static const char* const COLOR_NAMES[] = {
  "black", "white", "red", "green", "blue", "yellow",
  "cyan", "magenta", "orange", "navy", "darkgrey",
  "darkgray", "grey", "gray", NULL
};
static const uint16_t COLOR_VALS[] = {
  TFT_BLACK, TFT_WHITE, TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW,
  TFT_CYAN, TFT_MAGENTA, TFT_ORANGE, TFT_NAVY, TFT_DARKGREY,
  TFT_DARKGREY, TFT_DARKGREY, TFT_DARKGREY,
};

static uint8_t resolveColorIndex(const char* name) {
  if (!name || !name[0]) return 1; // default white
  for (int i = 0; COLOR_NAMES[i]; i++)
    if (strcmp(name, COLOR_NAMES[i]) == 0) return i;
  return 1;
}

static uint16_t resolveColorValue(uint8_t idx) {
  if (idx >= sizeof(COLOR_VALS)/sizeof(COLOR_VALS[0])) return TFT_WHITE;
  return COLOR_VALS[idx];
}

// ============================================================
// Value resolution helpers — extract typed values from JSON
// following the contract: {"slot":"name"}, {"lit":X}, {"noise":{lo,hi}}
// ============================================================
static int resolveSlotIndex(const JsonArray& state, const char* name) {
  if (!name) return -1;
  int i = 0;
  for (JsonObject s : state) {
    const char* n = s["name"];
    if (n && strcmp(n, name) == 0) return i;
    i++;
  }
  return -1;
}

// Read a value source from JSON: returns SRC_SLOT/SRC_LIT/SRC_FLOAT
// and fills slot_idx/lit_val/float_val.
static uint8_t parseSource(const JsonVariant& v, const JsonArray& stateDef,
                           uint8_t* out_slot, int32_t* out_lit, float* out_float) {
  *out_slot = 0xFF;
  *out_lit = 0;
  if (out_float) *out_float = 0.0f;

  if (v.isNull()) return SRC_LIT; // treat missing as 0

  if (v.is<JsonObject>()) {
    JsonObject o = v.as<JsonObject>();

    // {"slot":"name"}
    if (o["slot"].is<const char*>()) {
      int idx = resolveSlotIndex(stateDef, o["slot"]);
      if (idx >= 0 && idx < 256) {
        *out_slot = (uint8_t)idx;
        return SRC_SLOT;
      }
      return SRC_LIT; // unknown slot -> 0
    }

    // {"lit":X}
    if (o.containsKey("lit")) {
      JsonVariant lit = o["lit"];
      if (lit.is<float>()) {
        if (out_float) *out_float = lit.as<float>();
        *out_lit = (int32_t)lit.as<float>();
        return SRC_FLOAT;
      }
      *out_lit = lit.as<int64_t>();
      return SRC_LIT;
    }

    // {"noise":{"lo":...,"hi":...}}
    if (o.containsKey("noise")) {
      JsonObject n = o["noise"];
      // Pack lo into a_val, hi into b_val
      int32_t lo = (int32_t)(n["lo"].as<float>());
      int32_t hi = (int32_t)(n["hi"].as<float>());
      // We store lo in out_lit, return special marker — caller must check
      *out_lit = lo;
      if (out_float) *out_float = n["hi"].as<float>();
      return SRC_FLOAT; // hack: use float return to communicate hi
    }
    return SRC_LIT;
  }

  // Bare literal
  if (v.is<float>()) {
    if (out_float) *out_float = v.as<float>();
    *out_lit = (int32_t)v.as<float>();
    return SRC_FLOAT;
  }
  if (v.is<bool>()) {
    *out_lit = v.as<bool>() ? 1 : 0;
    return SRC_LIT;
  }
  *out_lit = v.as<int64_t>();
  return SRC_LIT;
}

// ============================================================
// Compile a guard JSON object → Guard struct
// ============================================================
static bool compileGuard(const JsonArray& rootState, const JsonVariant& gJson,
                         Guard* out) {
  memset(out, 0, sizeof(Guard));
  if (gJson.isNull()) { out->op = G_ALWAYS; return true; }

  JsonObject g = gJson.as<JsonObject>();
  if (!g || !g["op"]) { out->op = G_ALWAYS; return true; }

  const char* op = g["op"];
  if (strcmp(op, "always") == 0)            { out->op = G_ALWAYS; }
  else if (strcmp(op, "eq") == 0)           { out->op = G_EQ; }
  else if (strcmp(op, "ne") == 0)           { out->op = G_NE; }
  else if (strcmp(op, "lt") == 0)           { out->op = G_LT; }
  else if (strcmp(op, "le") == 0)           { out->op = G_LE; }
  else if (strcmp(op, "gt") == 0)           { out->op = G_GT; }
  else if (strcmp(op, "ge") == 0)           { out->op = G_GE; }
  else if (strcmp(op, "is_true") == 0)      { out->op = G_IS_TRUE; }
  else if (strcmp(op, "elapsed_ge") == 0)   { out->op = G_ELAPSED_GE; }
  else                                       { out->op = G_ALWAYS; }

  if (out->op == G_ALWAYS) return true;
  if (out->op == G_IS_TRUE) {
    out->a_ty = parseSource(g["a"], rootState, &out->a_slot, &out->a_val, NULL);
    return true;
  }
  out->a_ty = parseSource(g["a"], rootState, &out->a_slot, &out->a_val, NULL);
  out->b_ty = parseSource(g["b"], rootState, &out->b_slot, &out->b_val, NULL);
  return true;
}

// Forward decl
static bool compileBlock(const JsonArray& rootState, const JsonArray& block,
                         VmProgram* prog, uint8_t* out_start, uint8_t* out_count);

// ============================================================
// Compile a single verb → instruction(s)
// Returns number of instructions emitted (0 = skip)
// ============================================================
static int compileVerb(const JsonArray& rootState, const JsonObject& verb,
                       VmProgram* prog) {
  uint8_t n = prog->instr_count;
  if (n >= VM_MAX_INSTRUCTIONS - 4) return 0; // need room for worst case (if block)

  Instr* instr = &prog->instrs[n];
  memset(instr, 0, sizeof(Instr));

  // --- add ---
  if (verb.containsKey("add")) {
    JsonObject p = verb["add"];
    const char* slotName = p["slot"];
    int idx = resolveSlotIndex(rootState, slotName);
    if (idx < 0) return 0;

    // Check for noise source: add with noise is compiled as OP_NOISE
    // which does dst += random(lo, hi) in one instruction
    JsonVariant delta = p["delta"];
    if (delta.is<JsonObject>() && delta["noise"].is<JsonObject>()) {
      JsonObject n = delta["noise"];
      Instr* ni = &prog->instrs[prog->instr_count];
      memset(ni, 0, sizeof(Instr));
      ni->op = OP_NOISE;
      ni->dst = (uint8_t)idx;
      ni->a_val = (int32_t)(n["lo"].as<float>());
      ni->b_val = (int32_t)(n["hi"].as<float>());
      prog->instr_count++;
      return 1;
    }

    instr->op = OP_ADD;
    instr->dst = (uint8_t)idx;
    instr->a_ty = parseSource(p["delta"], rootState, &instr->a_slot,
                               &instr->a_val, NULL);
    prog->instr_count++;
    return 1;
  }

  // --- sub ---
  if (verb.containsKey("sub")) {
    JsonObject p = verb["sub"];
    const char* slotName = p["slot"];
    int idx = resolveSlotIndex(rootState, slotName);
    if (idx < 0) return 0;
    instr->op = OP_SUB;
    instr->dst = (uint8_t)idx;
    instr->a_ty = parseSource(p["delta"], rootState, &instr->a_slot,
                               &instr->a_val, NULL);
    prog->instr_count++;
    return 1;
  }

  // --- set ---
  if (verb.containsKey("set")) {
    JsonObject p = verb["set"];
    const char* slotName = p["slot"];
    int idx = resolveSlotIndex(rootState, slotName);
    if (idx < 0) return 0;
    instr->op = OP_SET;
    instr->dst = (uint8_t)idx;
    instr->a_ty = parseSource(p["value"], rootState, &instr->a_slot,
                               &instr->a_val, NULL);
    prog->instr_count++;
    return 1;
  }

  // --- setf (float set, same as set but preserves float type) ---
  if (verb.containsKey("setf")) {
    JsonObject p = verb["setf"];
    const char* slotName = p["slot"];
    int idx = resolveSlotIndex(rootState, slotName);
    if (idx < 0) return 0;
    instr->op = OP_SET;
    instr->dst = (uint8_t)idx;
    instr->a_ty = parseSource(p["value"], rootState, &instr->a_slot,
                               &instr->a_val, NULL);
    prog->instr_count++;
    return 1;
  }

  // --- toggle ---
  if (verb.containsKey("toggle")) {
    JsonObject p = verb["toggle"];
    const char* slotName = p["slot"];
    int idx = resolveSlotIndex(rootState, slotName);
    if (idx < 0) return 0;
    instr->op = OP_TOGGLE;
    instr->dst = (uint8_t)idx;
    prog->instr_count++;
    return 1;
  }

  // --- stamp ---
  if (verb.containsKey("stamp")) {
    JsonObject p = verb["stamp"];
    const char* slotName = p["slot"];
    int idx = resolveSlotIndex(rootState, slotName);
    if (idx < 0) return 0;
    instr->op = OP_STAMP;
    instr->dst = (uint8_t)idx;
    prog->instr_count++;
    return 1;
  }

  // --- now_minus ---
  if (verb.containsKey("now_minus")) {
    JsonObject p = verb["now_minus"];
    const char* slotName = p["slot"];
    int idx = resolveSlotIndex(rootState, slotName);
    if (idx < 0) return 0;
    instr->op = OP_NOW_MINUS;
    instr->dst = (uint8_t)idx;
    instr->a_ty = parseSource(p["from"], rootState, &instr->a_slot,
                               &instr->a_val, NULL);
    prog->instr_count++;
    return 1;
  }

  // --- format (op: ms, temp, react) ---
  if (verb.containsKey("format")) {
    JsonObject p = verb["format"];
    const char* slotName = p["slot"];
    const char* op = p["op"];
    int idx = resolveSlotIndex(rootState, slotName);
    if (idx < 0 || !op) return 0;
    if (strcmp(op, "ms") == 0)    instr->op = OP_FMT_MS;
    else if (strcmp(op, "temp") == 0)  instr->op = OP_FMT_TEMP;
    else if (strcmp(op, "react") == 0) instr->op = OP_FMT_REACT;
    else return 0;
    instr->dst = (uint8_t)idx;
    instr->a_ty = parseSource(p["src"], rootState, &instr->a_slot,
                               &instr->a_val, NULL);
    prog->instr_count++;
    return 1;
  }

  // --- progress ---
  if (verb.containsKey("progress")) {
    JsonObject p = verb["progress"];
    const char* slotName = p["slot"];
    int idx = resolveSlotIndex(rootState, slotName);
    if (idx < 0) return 0;
    instr->op = OP_PROGRESS;
    instr->dst = (uint8_t)idx;
    instr->a_ty = parseSource(p["src"], rootState, &instr->a_slot,
                               &instr->a_val, NULL);
    // Pack max into b_val, px into a_val
    int32_t maxVal = 1;
    parseSource(p["max"], rootState, &instr->b_slot, &maxVal, NULL);
    instr->b_val = maxVal;
    int32_t pxVal = 220;
    parseSource(p["px"], rootState, &instr->a_slot, &pxVal, NULL);
    instr->a_val = pxVal;
    prog->instr_count++;
    return 1;
  }

  // --- render ---
  if (verb.containsKey("render")) {
    JsonObject p = verb["render"];
    uint8_t ri = prog->render_count;
    if (ri >= VM_MAX_RENDER) return 0;
    RenderPrim* rp = &prog->renders[ri];
    memset(rp, 0, sizeof(RenderPrim));

    const char* type = p["type"];
    if (!type) return 0;
    if (strcmp(type, "clear") == 0)          rp->type = R_CLEAR;
    else if (strcmp(type, "text") == 0)      rp->type = R_TEXT;
    else if (strcmp(type, "rect") == 0)      rp->type = R_RECT;
    else if (strcmp(type, "fillrect") == 0)  rp->type = R_FILLRECT;
    else if (strcmp(type, "line") == 0)      rp->type = R_LINE;
    else if (strcmp(type, "circle") == 0)    rp->type = R_CIRCLE;
    else if (strcmp(type, "bar") == 0)       rp->type = R_BAR;
    else if (strcmp(type, "graph") == 0)     rp->type = R_GRAPH;
    else return 0;

    // Colour
    rp->color_slot = 0xFF;   // default: use color_idx (literal), not a slot
    if (p["color"].is<JsonObject>() && p["color"]["slot"].is<const char*>()) {
      JsonObject c = p["color"];
      int sidx = resolveSlotIndex(rootState, c["slot"]);
      if (sidx >= 0) { rp->color_slot = (uint8_t)sidx; rp->color_idx = 0xFF; }
    } else if (p["color"].is<const char*>()) {
      rp->color_idx = resolveColorIndex(p["color"]);
    } else {
      rp->color_idx = 1; // white default
    }

    // Position/size — can be slot refs or literals
    auto parseCoord = [&](const char* key, int16_t* out_val, uint8_t* out_slot) {
      JsonVariant v = p[key];
      if (v.isNull()) return;
      if (v.is<JsonObject>()) {
        JsonObject o = v.as<JsonObject>();
        if (o["slot"].is<const char*>()) {
          int si = resolveSlotIndex(rootState, o["slot"]);
          if (si >= 0) { *out_slot = (uint8_t)si; *out_val = 0; }
        } else if (o.containsKey("lit")) {
          *out_val = (int16_t)o["lit"].as<int64_t>();
          *out_slot = 0xFF;
        }
      } else {
        *out_val = (int16_t)v.as<int64_t>();
        *out_slot = 0xFF;
      }
    };
    parseCoord("x", &rp->x, &rp->x_slot);
    parseCoord("y", &rp->y, &rp->y_slot);
    parseCoord("w", &rp->w, &rp->w_slot);
    parseCoord("h", &rp->h, &rp->h_slot);
    // Circle uses 'r' — store in w field
    if (p.containsKey("r") && rp->type == R_CIRCLE)
      parseCoord("r", &rp->w, &rp->w_slot);

    // Text source
    if (p["src"].is<JsonObject>() && p["src"]["slot"].is<const char*>()) {
      int si = resolveSlotIndex(rootState, p["src"]["slot"]);
      if (si >= 0) rp->src_slot = (uint8_t)si;
    } else if (p["src"].is<JsonObject>() && p["src"]["lit"].is<const char*>()) {
      strncpy(rp->text, p["src"]["lit"], VM_STR_LEN - 1);
      rp->src_slot = 0xFF;
    } else if (p["src"].is<const char*>()) {
      strncpy(rp->text, p["src"], VM_STR_LEN - 1);
      rp->src_slot = 0xFF;
    } else if (p["slot"].is<JsonObject>() && p["slot"]["slot"].is<const char*>()) {
      // graph: {"slot":{"slot":"temp"}}
      int si = resolveSlotIndex(rootState, p["slot"]["slot"]);
      if (si >= 0) rp->src_slot = (uint8_t)si;
    }

    // Font
    uint8_t fontVal = 2;
    JsonVariant fv = p["font"];
    if (fv.is<JsonObject>()) {
      if (fv["lit"].is<const char*>()) fontVal = (uint8_t)atoi(fv["lit"]);
      else if (fv["lit"].is<int64_t>()) fontVal = (uint8_t)(int64_t)fv["lit"];
      else if (fv["lit"].is<int>()) fontVal = (uint8_t)(int)fv["lit"];
    } else if (fv.is<int64_t>()) {
      fontVal = (uint8_t)(int64_t)fv;
    } else if (fv.is<const char*>()) {
      fontVal = (uint8_t)atoi(fv);
    }
    rp->font = fontVal;
    if (rp->font < 1) rp->font = 2;
    if (rp->font > 7) rp->font = 7;

    prog->render_count++;
    instr->op = OP_RENDER;
    instr->dst = ri; // render primitive index
    prog->instr_count++;
    return 1;
  }

  // --- if ---
  if (verb.containsKey("if")) {
    JsonObject p = verb["if"];
    uint8_t gi = prog->guard_count;
    if (gi >= VM_MAX_GUARDS) return 0;
    if (!compileGuard(rootState, p["guard"], &prog->guards[gi]))
      return 0;
    prog->guard_count++;

    // Emit OP_IF FIRST (then/else blocks follow it in the stream):
    //   [OP_IF (guard gi, a_val=then_count, b_val=else_count)]
    //   [then instructions...]
    //   [else instructions...]
    uint8_t if_idx = prog->instr_count;
    Instr* ii = &prog->instrs[if_idx];
    memset(ii, 0, sizeof(Instr));
    ii->op = OP_IF;
    ii->dst = gi;           // guard index
    prog->instr_count++;

    // Compile 'then' block (immediately after OP_IF)
    uint8_t then_start = 0, then_count = 0;
    compileBlock(rootState, p["then"], prog, &then_start, &then_count);

    // Compile 'else' block (may be absent)
    uint8_t else_start = 0, else_count = 0;
    if (p["else"].is<JsonArray>())
      compileBlock(rootState, p["else"], prog, &else_start, &else_count);

    ii->a_val = then_count; // number of instructions in 'then'
    ii->b_val = else_count; // number of instructions in 'else'

    // The handler count must account for OP_IF + then + else
    return 1 + then_count + else_count;
  }

  return 0; // unknown verb — skip
}

// ============================================================
// Compile a block (JSON array of verb objects) → instructions
// ============================================================
static bool compileBlock(const JsonArray& rootState, const JsonArray& block,
                         VmProgram* prog, uint8_t* out_start, uint8_t* out_count) {
  if (!block) { *out_start = 0; *out_count = 0; return true; }
  *out_start = prog->instr_count;
  *out_count = 0;
  for (JsonObject verb : block) {
    int n = compileVerb(rootState, verb, prog);
    *out_count += n;
  }
  return true;
}

// ============================================================
// Public API: vmCompile — compile JSON behavior into VmProgram
// ============================================================
bool vmCompile(const JsonObject& root, VmProgram* prog) {
  memset(prog, 0, sizeof(VmProgram));

  // --- ID ---
  const char* id = root["id"];
  if (!id) return false;
  strncpy(prog->id, id, sizeof(prog->id) - 1);

  JsonArray stateDef = root["state"];

  // --- State slots ---
  if (stateDef) {
    for (JsonObject s : stateDef) {
      uint8_t i = prog->slot_count;
      if (i >= VM_MAX_SLOTS) break;
      const char* name = s["name"];
      const char* type = s["type"];
      if (!name || !type) continue;

      strncpy(prog->slots[i].name, name, sizeof(prog->slots[i].name) - 1);
      if (strcmp(type, "int64") == 0) {
        prog->slots[i].type = SLOT_INT64;
        prog->slots[i].init_i = s["init"] | (int64_t)0;
      } else if (strcmp(type, "float") == 0) {
        prog->slots[i].type = SLOT_FLOAT;
        prog->slots[i].init_f = s["init"].is<float>() ? (float)s["init"] : 0.0f;
      } else if (strcmp(type, "bool") == 0) {
        prog->slots[i].type = SLOT_BOOL;
        prog->slots[i].init_i = (bool)(s["init"] | false) ? 1 : 0;
      } else if (strcmp(type, "string") == 0) {
        prog->slots[i].type = SLOT_STRING;
        strncpy(prog->slots[i].init_s, s["init"] | "", VM_STR_LEN - 1);
      } else {
        prog->slots[i].type = SLOT_INT64;
        prog->slots[i].init_i = s["init"] | (int64_t)0;
      }
      prog->slot_count++;
    }
  }

  // --- Events ---
  JsonArray events = root["events"];
  if (events) {
    for (JsonObject ev : events) {
      uint8_t ei = prog->event_count;
      if (ei >= VM_MAX_EVENTS) break;

      const char* evName = ev["name"];
      if (evName) strncpy(prog->events[ei].name, evName,
                          sizeof(prog->events[ei].name) - 1);

      // Schedule
      JsonObject sched = ev["schedule"];
      if (sched) {
        const char* stype = sched["type"];
        if (stype && strcmp(stype, "interval") == 0) {
          prog->events[ei].interval_ms = sched["ms"] | 1000;
        } else {
          prog->events[ei].interval_ms = 0; // once
        }
      } else {
        prog->events[ei].interval_ms = -1; // no schedule (touch event)
      }

      // Handlers
      JsonArray handlers = ev["handlers"];
      if (handlers) {
        for (JsonObject h : handlers) {
          uint8_t hi = prog->events[ei].handler_count;
          if (hi >= VM_MAX_HANDLERS) break;

          // Compile guard
          uint8_t gi = prog->guard_count;
          if (gi >= VM_MAX_GUARDS) break;
          if (!compileGuard(stateDef, h["guard"], &prog->guards[gi]))
            continue;
          prog->guard_count++;
          prog->events[ei].handlers[hi].guard_idx = gi;

          // Compile run block
          compileBlock(stateDef, h["run"], prog,
                       &prog->events[ei].handlers[hi].instr_start,
                       &prog->events[ei].handlers[hi].instr_count);

          prog->events[ei].handler_count++;
        }
      }
      prog->event_count++;
    }
  }

  return true;
}

// ============================================================
// Public API: vmStateInit — initialize runtime state
// ============================================================
void vmStateInit(VmState* state, const VmProgram* prog) {
  memset(state, 0, sizeof(VmState));
  for (uint8_t i = 0; i < prog->slot_count; i++) {
    state->types[i] = prog->slots[i].type;
    switch (prog->slots[i].type) {
      case SLOT_INT64: state->ival[i] = prog->slots[i].init_i; break;
      case SLOT_FLOAT: state->fval[i] = prog->slots[i].init_f; break;
      case SLOT_BOOL:  state->ival[i] = prog->slots[i].init_i; break;
      case SLOT_STRING:
        strncpy(state->sval[i], prog->slots[i].init_s, VM_STR_LEN - 1);
        break;
    }
  }
  state->slot_count = prog->slot_count;
  // event_last already zeroed by memset
}

// ============================================================
// Executor helpers
// ============================================================

// Read a source operand: returns int64
static int64_t srcInt(const VmState* state, uint8_t ty, uint8_t slot, int32_t val) {
  if (ty == SRC_SLOT) {
    if (state->types[slot] == SLOT_FLOAT) return (int64_t)state->fval[slot];
    return state->ival[slot];
  }
  return (int64_t)val;
}

// Read a source operand: returns float
static float srcFloat(const VmState* state, uint8_t ty, uint8_t slot, int32_t val) {
  if (ty == SRC_SLOT) {
    if (state->types[slot] == SLOT_FLOAT) return state->fval[slot];
    return (float)state->ival[slot];
  }
  return (float)val;
}

// ============================================================
// Evaluate a guard
// ============================================================
static bool evalGuard(const VmState* state, const Guard* g, unsigned long now) {
  switch (g->op) {
    case G_ALWAYS: return true;

    case G_EQ: {
      float a = srcFloat(state, g->a_ty, g->a_slot, g->a_val);
      float b = srcFloat(state, g->b_ty, g->b_slot, g->b_val);
      // String comparison if either source is a string slot
      if (g->a_ty == SRC_SLOT && state->types[g->a_slot] == SLOT_STRING)
        return strcmp(state->sval[g->a_slot], state->sval[g->b_slot]) == 0;
      return a == b;
    }
    case G_NE: {
      float a = srcFloat(state, g->a_ty, g->a_slot, g->a_val);
      float b = srcFloat(state, g->b_ty, g->b_slot, g->b_val);
      return a != b;
    }
    case G_LT: {
      float a = srcFloat(state, g->a_ty, g->a_slot, g->a_val);
      float b = srcFloat(state, g->b_ty, g->b_slot, g->b_val);
      return a < b;
    }
    case G_LE: {
      float a = srcFloat(state, g->a_ty, g->a_slot, g->a_val);
      float b = srcFloat(state, g->b_ty, g->b_slot, g->b_val);
      return a <= b;
    }
    case G_GT: {
      float a = srcFloat(state, g->a_ty, g->a_slot, g->a_val);
      float b = srcFloat(state, g->b_ty, g->b_slot, g->b_val);
      return a > b;
    }
    case G_GE: {
      float a = srcFloat(state, g->a_ty, g->a_slot, g->a_val);
      float b = srcFloat(state, g->b_ty, g->b_slot, g->b_val);
      return a >= b;
    }
    case G_IS_TRUE:
      return srcInt(state, g->a_ty, g->a_slot, g->a_val) != 0;

    case G_ELAPSED_GE: {
      int64_t stamp = srcInt(state, g->a_ty, g->a_slot, g->a_val);
      int64_t ms = srcInt(state, g->b_ty, g->b_slot, g->b_val);
      return stamp > 0 && (now - (unsigned long)stamp) >= (unsigned long)ms;
    }
  }
  return false;
}

// ============================================================
// Execute a single instruction against runtime state
// Returns number of instructions to skip (for OP_IF)
// ============================================================
static void execRange(VmState* state, const VmProgram* prog,
                      uint8_t start, uint8_t count, unsigned long now);

static int execInstr(VmState* state, const VmProgram* prog,
                     const Instr* instr, unsigned long now) {
  switch (instr->op) {
    case OP_NOP: return 1;

    case OP_ADD: {
      if (state->types[instr->dst] == SLOT_FLOAT)
        state->fval[instr->dst] += srcFloat(state, instr->a_ty, instr->a_slot, instr->a_val);
      else
        state->ival[instr->dst] += srcInt(state, instr->a_ty, instr->a_slot, instr->a_val);
      return 1;
    }

    case OP_SUB:
      state->ival[instr->dst] -= srcInt(state, instr->a_ty, instr->a_slot, instr->a_val);
      return 1;

    case OP_SET: {
      if (instr->a_ty == SRC_SLOT && state->types[instr->a_slot] == SLOT_STRING &&
          state->types[instr->dst] == SLOT_STRING) {
        strncpy(state->sval[instr->dst], state->sval[instr->a_slot], VM_STR_LEN - 1);
      } else if (state->types[instr->dst] == SLOT_FLOAT) {
        state->fval[instr->dst] = srcFloat(state, instr->a_ty, instr->a_slot, instr->a_val);
      } else {
        state->ival[instr->dst] = srcInt(state, instr->a_ty, instr->a_slot, instr->a_val);
      }
      return 1;
    }

    case OP_TOGGLE:
      state->ival[instr->dst] = !state->ival[instr->dst];
      return 1;

    case OP_STAMP:
      state->ival[instr->dst] = (int64_t)now;
      return 1;

    case OP_NOW_MINUS:
      state->ival[instr->dst] = (int64_t)(now - (unsigned long)
        srcInt(state, instr->a_ty, instr->a_slot, instr->a_val));
      return 1;

    case OP_NOISE: {
      int32_t lo = instr->a_val;
      int32_t hi = instr->b_val;
      // dst += random(lo, hi) — additive noise for temp-logger
      float noise = ((float)random(0, 100) / 100.0f) * (float)(hi - lo) + (float)lo;
      if (state->types[instr->dst] == SLOT_FLOAT)
        state->fval[instr->dst] += noise;
      else
        state->ival[instr->dst] += (int64_t)noise;
      return 1;
    }

    case OP_FMT_MS: {
      int64_t ms = srcInt(state, instr->a_ty, instr->a_slot, instr->a_val);
      char b[12];
      snprintf(b, sizeof(b), "%02lld:%02lld", ms / 60000, (ms % 60000) / 1000);
      strncpy(state->sval[instr->dst], b, VM_STR_LEN - 1);
      return 1;
    }

    case OP_FMT_TEMP: {
      float t = srcFloat(state, instr->a_ty, instr->a_slot, instr->a_val);
      char b[16];
      snprintf(b, sizeof(b), "%.1f F", t);
      strncpy(state->sval[instr->dst], b, VM_STR_LEN - 1);
      return 1;
    }

    case OP_FMT_REACT: {
      int64_t ms = srcInt(state, instr->a_ty, instr->a_slot, instr->a_val);
      char b[16];
      snprintf(b, sizeof(b), "%lld ms", ms);
      strncpy(state->sval[instr->dst], b, VM_STR_LEN - 1);
      return 1;
    }

    case OP_PROGRESS: {
      int64_t src = srcInt(state, instr->a_ty, instr->a_slot, instr->a_val);
      int64_t maxVal = instr->b_val;
      int64_t pxVal = instr->a_val;
      if (maxVal <= 0) maxVal = 1;
      int64_t w = (src % maxVal) * pxVal / maxVal;
      state->ival[instr->dst] = w;
      return 1;
    }

    case OP_RENDER:
      // Mark this render primitive dirty — vmRender() draws only dirty ones
      state->render_dirty |= (1u << instr->dst);
      return 1;

    case OP_IF: {
      const Guard* g = &prog->guards[instr->dst];
      // Layout: [OP_IF][then: a_val instrs][else: b_val instrs]
      uint8_t base = (uint8_t)(instr - prog->instrs);
      if (evalGuard(state, g, now)) {
        // Execute 'then' block (immediately following this instr)
        execRange(state, prog, base + 1, instr->a_val, now);
      } else {
        // Execute 'else' block (after the then block)
        execRange(state, prog, base + 1 + instr->a_val, instr->b_val, now);
      }
      // Skip OP_IF + then + else entirely
      return 1 + instr->a_val + instr->b_val;
    }

    default:
      return 1;
  }
}

// ============================================================
// Execute a range of instructions (for a handler block)
// ============================================================
static void execRange(VmState* state, const VmProgram* prog,
                      uint8_t start, uint8_t count, unsigned long now) {
  uint8_t pc = start;
  uint8_t end = start + count;
  while (pc < end) {
    int skip = execInstr(state, prog, &prog->instrs[pc], now);
    if (skip <= 0) skip = 1;
    pc += skip;
    if (pc > end) break;
  }
}

// ============================================================
// Public API: vmTick — check scheduled events, execute if due
// Returns true if any handler executed (caller should redraw)
// ============================================================
bool vmTick(VmState* state, const VmProgram* prog, unsigned long now) {
  bool ran = false;
  for (uint8_t ei = 0; ei < prog->event_count; ei++) {
    const VmEvent* ev = &prog->events[ei];
    if (ev->interval_ms <= 0) continue; // not scheduled (touch-only)

    if (now - state->event_last[ei] >= (unsigned long)ev->interval_ms) {
      state->event_last[ei] = now;

      // First-match-guard-wins handler dispatch
      for (uint8_t hi = 0; hi < ev->handler_count; hi++) {
        const Handler* h = &ev->handlers[hi];
        const Guard* g = &prog->guards[h->guard_idx];
        if (!evalGuard(state, g, now)) continue;

        // Execute this handler's block
        execRange(state, prog, h->instr_start, h->instr_count, now);
        ran = true;
        break; // first match wins
      }
    }
  }
  return ran;
}

// ============================================================
// Public API: vmDispatch — dispatch a named event (e.g. touch)
// ============================================================
bool vmDispatch(VmState* state, const VmProgram* prog, const char* eventName) {
  for (uint8_t ei = 0; ei < prog->event_count; ei++) {
    if (strcmp(prog->events[ei].name, eventName) != 0) continue;
    const VmEvent* ev = &prog->events[ei];

    unsigned long now = millis();

    // First-match-guard-wins handler dispatch
    for (uint8_t hi = 0; hi < ev->handler_count; hi++) {
      const Handler* h = &ev->handlers[hi];
      const Guard* g = &prog->guards[h->guard_idx];
      if (!evalGuard(state, g, now)) continue;

      execRange(state, prog, h->instr_start, h->instr_count, now);
      return true; // handled
    }
    return false; // event found but no handler matched
  }
  return false; // event not found
}

// ============================================================
// Public API: vmRender — draw all render primitives
// (Called after vmTick / vmDispatch to flush the display)
// ============================================================
void vmRender(VmState* state, const VmProgram* prog, TFT_eSPI* tft) {
  for (uint8_t i = 0; i < prog->render_count; i++) {
    // Only draw primitives whose handler executed this frame
    if (!(state->render_dirty & (1u << i))) continue;
    const RenderPrim* rp = &prog->renders[i];

    // Resolve colour
    uint16_t color;
    if (rp->color_slot != 0xFF && rp->color_slot < state->slot_count) {
      color = resolveColorValue(resolveColorIndex(state->sval[rp->color_slot]));
    } else {
      color = resolveColorValue(rp->color_idx);
    }

    // Resolve coordinates
    int16_t x = (rp->x_slot != 0xFF) ? (int16_t)state->ival[rp->x_slot] : rp->x;
    int16_t y = (rp->y_slot != 0xFF) ? (int16_t)state->ival[rp->y_slot] : rp->y;
    int16_t w = (rp->w_slot != 0xFF) ? (int16_t)state->ival[rp->w_slot] : rp->w;
    int16_t h = (rp->h_slot != 0xFF) ? (int16_t)state->ival[rp->h_slot] : rp->h;

    // Resolve text source
    const char* text = NULL;
    char textBuf[VM_STR_LEN];
    if (rp->src_slot != 0xFF) {
      text = state->sval[rp->src_slot];
    } else if (rp->text[0]) {
      text = rp->text;
    }

    switch (rp->type) {
      case R_CLEAR:
        tft->fillScreen(color);
        break;

      case R_TEXT: {
        uint8_t f = rp->font;
        if (f < 1) f = 2;
        if (f > 7) f = 7;
        tft->setTextColor(color, TFT_BLACK);
        tft->drawString(text ? text : "", x, y, f);
        break;
      }

      case R_RECT:
        tft->drawRect(x, y, w, h, color);
        break;

      case R_FILLRECT:
        tft->fillRect(x, y, w, h, color);
        break;

      case R_LINE:
        tft->drawLine(x, y, w, h, color);
        break;

      case R_CIRCLE:
        tft->fillCircle(x, y, w > 0 ? w : 10, color);
        break;

      case R_BAR:
        if (w > 0) tft->fillRect(x, y, w, h, color);
        tft->drawRect(x, y, w, h, color);
        break;

      case R_GRAPH: {
        // Rolling line graph (temp-logger)
        float currentTemp = state->fval[rp->src_slot];
        int gh = state->graph_head;
        state->graph_hist[gh] = currentTemp;
        state->graph_head = (gh + 1) % VM_GRAPH_SIZE;
        if (state->graph_count < VM_GRAPH_SIZE) state->graph_count++;

        int16_t gx = x, gy = y, gw = w, gh_h = h;
        if (gw <= 0) { gx = 10; gw = 220; }
        if (gh_h <= 0) { gy = 100; gh_h = 180; }
        int16_t botY = gy + gh_h;

        tft->drawRect(gx, gy, gw, gh_h, TFT_DARKGREY);

        int n = state->graph_count;
        if (n > 1) {
          int drawW = gw / (n - 1);
          if (drawW < 1) drawW = 1;
          if (drawW > 4) drawW = 4;
          int start = (state->graph_head - n + VM_GRAPH_SIZE) % VM_GRAPH_SIZE;
          for (int i = 0; i < n - 1; i++) {
            int idx0 = (start + i) % VM_GRAPH_SIZE;
            int idx1 = (start + i + 1) % VM_GRAPH_SIZE;
            float v0 = state->graph_hist[idx0], v1 = state->graph_hist[idx1];
            int y0 = botY - (int)(((v0 - 60.0f) / 30.0f) * gh_h);
            int y1 = botY - (int)(((v1 - 60.0f) / 30.0f) * gh_h);
            int x0 = gx + i * drawW, x1 = gx + (i + 1) * drawW;
            if (x0 >= gx && x1 <= gx + gw)
              tft->drawLine(x0, y0, x1, y1, TFT_GREEN);
          }
        }
        break;
      }
    }
  }
  state->render_dirty = 0;   // consume the dirty set — next frame only what runs
}

// ============================================================
// Slot read helpers
// ============================================================
int64_t vmGetInt(const VmState* state, uint8_t idx) {
  if (idx >= state->slot_count) return 0;
  if (state->types[idx] == SLOT_FLOAT) return (int64_t)state->fval[idx];
  return state->ival[idx];
}
float vmGetFloat(const VmState* state, uint8_t idx) {
  if (idx >= state->slot_count) return 0.0f;
  if (state->types[idx] == SLOT_FLOAT) return state->fval[idx];
  return (float)state->ival[idx];
}
const char* vmGetStr(const VmState* state, uint8_t idx) {
  if (idx >= state->slot_count) return "";
  if (state->types[idx] == SLOT_STRING) return state->sval[idx];
  return "";
}
bool vmGetBool(const VmState* state, uint8_t idx) {
  if (idx >= state->slot_count) return false;
  return state->ival[idx] != 0;
}