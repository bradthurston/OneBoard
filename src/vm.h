/* ==========================================================
   OneBoard Stage 3 — Tiny compiled VM for behavior execution
   Replaces raw-JSON-at-runtime interpreter with JSON→bytecode
   compilation at load time, reducing RAM and CPU overhead.
   ========================================================== */
#ifndef VM_H
#define VM_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

// ============================================================
// Limits — comfortably fit any behavior (timer, temp-logger,
// reaction-game each use <10 slots, <50 instrs, <3 events)
// ============================================================
#define VM_MAX_SLOTS       12
#define VM_MAX_EVENTS       6
#define VM_MAX_HANDLERS     6     // per event
#define VM_MAX_INSTRUCTIONS 80
#define VM_MAX_GUARDS       12
#define VM_MAX_RENDER       20
#define VM_STR_LEN          20    // string slot capacity
#define VM_GRAPH_SIZE       100   // FIFO ring for graph render

// ============================================================
// Slot types
// ============================================================
#define SLOT_NONE    0
#define SLOT_INT64   1
#define SLOT_FLOAT   2
#define SLOT_BOOL    3
#define SLOT_STRING  4

// ============================================================
// Operand source types
// ============================================================
#define SRC_UNUSED  0   // not used
#define SRC_SLOT    1   // value comes from a state slot
#define SRC_LIT     2   // value is a literal int32
#define SRC_FLOAT   3   // value is a literal float
#define SRC_LIT_MS  4   // literal interpreted as milliseconds (same as SRC_LIT)

// ============================================================
// Opcodes — instruction types for the VM
// ============================================================
enum VmOp : uint8_t {
  OP_NOP = 0,

  // --- State manipulation (dst = slot index, a = source) ---
  OP_ADD,        // dst += a
  OP_SUB,        // dst -= a
  OP_SET,        // dst = a
  OP_TOGGLE,     // dst = !dst
  OP_STAMP,      // dst = millis()
  OP_NOW_MINUS,  // dst = millis() - a
  OP_NOISE,      // dst = random(a_val..b_val)

  // --- Formatting (write to string dst slot) ---
  OP_FMT_MS,     // dst = format_ms(a) e.g. "01:23"
  OP_FMT_TEMP,   // dst = format_temp(a) e.g. "72.5 F"
  OP_FMT_REACT,  // dst = format_react(a) e.g. "342 ms"

  // --- Progress ---
  OP_PROGRESS,   // dst = (a % b_lit) * c_lit / b_lit  (px=max, max=total)

  // --- Render ---
  OP_RENDER,     // render(dst) where dst = render_primitive index

  // --- Control (dst = guard_index) ---
  OP_IF,         // if(g[dst]) { a_lit then-instrs } [else { b_lit else-instrs }]
};

// ============================================================
// Guard opcodes
// ============================================================
enum VmGuard : uint8_t {
  G_ALWAYS = 0,
  G_EQ, G_NE, G_LT, G_LE, G_GT, G_GE,
  G_IS_TRUE,
  G_ELAPSED_GE,
};

// ============================================================
// Render primitive types
// ============================================================
enum RenderType : uint8_t {
  R_CLEAR = 0,
  R_TEXT,
  R_RECT,
  R_FILLRECT,
  R_LINE,
  R_CIRCLE,
  R_BAR,
  R_GRAPH,
};

// ============================================================
// Packed instruction — 12 bytes
// ============================================================
struct __attribute__((packed)) Instr {
  uint8_t op;       // VmOp
  uint8_t dst;      // destination slot or render index
  uint8_t a_ty;     // SRC_* for operand A
  uint8_t a_slot;   // slot index (when a_ty == SRC_SLOT)
  int32_t a_val;    // literal value (when a_ty == SRC_LIT)
  uint8_t b_ty;
  uint8_t b_slot;
  int32_t b_val;
};

// ============================================================
// Guard — 12 bytes
// ============================================================
struct __attribute__((packed)) Guard {
  uint8_t op;       // VmGuard
  uint8_t a_ty;
  uint8_t a_slot;
  int32_t a_val;
  uint8_t b_ty;
  uint8_t b_slot;
  int32_t b_val;
};

// ============================================================
// Handler — pair of guard + instruction block
// ============================================================
struct Handler {
  uint8_t guard_idx;     // index into program.guards[]
  uint8_t instr_start;   // index into program.instrs[]
  uint8_t instr_count;   // number of instructions in block
};

// ============================================================
// Event — schedule + ordered handlers
// ============================================================
struct VmEvent {
  char name[16];
  int32_t interval_ms;   // -1 = no schedule (touch/trigger-only), >0 = periodic
  uint8_t handler_count;
  Handler handlers[VM_MAX_HANDLERS];
};

// ============================================================
// Render primitive — pre-parsed from JSON render verb
// Coords/sizes use slot refs for dynamic values (e.g. progress bar width)
// ============================================================
struct __attribute__((packed)) RenderPrim {
  uint8_t type;           // R_*
  uint8_t color_idx;      // resolved colour index (0-14), 0xFF = use slot
  uint8_t color_slot;     // slot containing colour name string
  int16_t x, y, w, h;    // defaults — used when no slot ref
  uint8_t x_slot, y_slot, w_slot, h_slot;  // 0xFF = use default above
  uint8_t src_slot;       // text source slot (0xFF = use text_literal)
  uint8_t font;
  char text[VM_STR_LEN];  // inline text when src_slot==0xFF
};

// ============================================================
// Slot definition (from JSON state array)
// ============================================================
struct SlotDef {
  char name[16];
  uint8_t type;           // SLOT_*
  int64_t init_i;         // initial int64 value
  float   init_f;         // initial float value
  char    init_s[VM_STR_LEN];  // initial string value
};

// ============================================================
// Compiled program — output of the JSON compiler, read-only at runtime
// ============================================================
struct VmProgram {
  char id[16];

  // State template
  uint8_t slot_count;
  SlotDef slots[VM_MAX_SLOTS];

  // Events
  uint8_t event_count;
  VmEvent events[VM_MAX_EVENTS];

  // Shared instruction pool (all handlers' blocks concatenated)
  uint8_t instr_count;
  Instr instrs[VM_MAX_INSTRUCTIONS];

  // Shared guard pool
  uint8_t guard_count;
  Guard guards[VM_MAX_GUARDS];

  // Shared render primitive pool
  uint8_t render_count;
  RenderPrim renders[VM_MAX_RENDER];
};

// ============================================================
// Runtime VM state — mutable, one instance
// ============================================================
struct VmState {
  // Slot storage
  int64_t ival[VM_MAX_SLOTS];            // int64 + bool storage
  float   fval[VM_MAX_SLOTS];            // float storage
  char    sval[VM_MAX_SLOTS][VM_STR_LEN]; // string storage
  uint8_t types[VM_MAX_SLOTS];           // SLOT_*
  uint8_t slot_count;                    // number of active slots

  // Graph ring buffer (for R_GRAPH)
  float   graph_hist[VM_GRAPH_SIZE];
  int     graph_head;
  int     graph_count;

  // Per-event timer tracking
  unsigned long event_last[VM_MAX_EVENTS];
};

// ============================================================
// API
// ============================================================

// Compile JSON behavior into a VmProgram. Returns true on success.
// Uses the provided JsonDocument (already parsed). After this call
// the caller may reuse/free the JsonDocument.
bool vmCompile(const JsonObject& root, VmProgram* prog);

// Initialize runtime state from a compiled program (sets initial values)
void vmStateInit(VmState* state, const VmProgram* prog);

// Check scheduled events and execute if due. Call from loop().
void vmTick(VmState* state, const VmProgram* prog, unsigned long now);

// Dispatch a named event (e.g. "touch-down"). Returns true if handled.
bool vmDispatch(VmState* state, const VmProgram* prog, const char* eventName);

// Execute all render primitives against the TFT display.
// State may be mutated (graph ring buffer updates).
void vmRender(VmState* state, const VmProgram* prog, TFT_eSPI* tft);

// Slot read helpers (for external use e.g. web status)
int64_t     vmGetInt(const VmState* state, uint8_t idx);
float       vmGetFloat(const VmState* state, uint8_t idx);
const char* vmGetStr(const VmState* state, uint8_t idx);
bool        vmGetBool(const VmState* state, uint8_t idx);

#endif // VM_H