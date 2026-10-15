# Stage 3 — VM compile design (GPT design, reviewed & accepted; schema of record)

**Design contributor:** ChatGPT (paid consulting sub). Reviewed + compatibility-verified by Bob
against the Stage 2 interpreter. Accepted for Stage 3 dispatch.

## Design intent (GPT, verbatim)

"Use a compact, typed instruction representation — not a tightly packed byte stream. Compile names
to numeric IDs, flatten render parameters into descriptors, then release the JSON document. That
preserves the existing JSON while removing parsing, string lookup, and allocation from execution."

**Compile into one immutable program at load (contract point 1), using these tables:**

| Table | Contents |
|-------|----------|
| Slots | Type, initial value, bounded string-buffer location |
| Events | Event ID, handler range, optional interval/touch configuration |
| Handlers | Optional guard ID, instruction start, instruction count |
| Guards | Comparison opcode and operand IDs |
| Instructions | Fixed-size instructions |
| Operands | Literal, slot reference, or noise source |
| Constants | Integer/float values and string references |
| Render descriptors | Primitive type and flattened operand IDs |
| Strings | Immutable literal text in one bounded arena |

Model: compile the JSON once at load → immutable program in SRAM → release the JsonDocument.
Execution walks fixed-size instructions with no parsing / string lookup / allocation. This directly
satisfies contract points 1 (same JSON, compile at load), 2 (typed slots, no dyn vars), 3 (events →
handlers → guards), 8 (slot refs, structured guards, no string eval).

## The three "resolve before dispatch" details (GPT) — verified compatible

1. **Render instructions live INSIDE handlers** (not a separate declarative view). Stage 3 must
   support that existing model. **Verified:** all render verbs are inline in handler `run` lists
   across timer/temp-logger/reaction-game (25 occurrences, none outside handlers). Keep `render` as
   an instruction in the handler instruction region; flatten its params into a render descriptor.

2. **`if` exists despite the straight-line contract.** Define as **bounded forward branching only,
   no backward jumps.** **Verified:** current `if` dispatches a contained then/else block on guard
   match; there is no backward-jump or loop primitive in the interpreter (the only `while` in
   `main.cpp` is the LittleFS file read, not behavior logic). The new IF/BEQ instruction may only
   branch forward within the handler's instruction range; a backward target is a schema violation.

3. **`{"noise":...}` is an executable value source.** Specify when it is sampled; preserve Stage 2
   behavior (per-execution sampling), do NOT turn it into a load-time constant. **Verified:** both
   `varInt` and `varFloat` sample `random(...)` on every evaluation, per use — the temp-logger's
   animated wobble depends on per-tick resampling. The compiled operand must carry a NOISE relocator
   that re-samples each execution, not a constant folded at compile time.

## Instruction/opcode set (fixed ~20-30 verbs, contract-compatible)

Grouped from the Stage 2 verb vocab + guards + values:

- **Arithmetic/move:** ADD, SUB, SET, SETF, TOGGLE, STAMP, NOW_MINUS
- **Derived:** FORMAT (op ms/temp → string slot), PROGRESS (src/max/px → width slot)
- **Render:** RENDER with descriptor index (clear/text/rect/fillrect/line/circle/bar/graph)
- **Control:** IF/BEQ — bounded forward branch (see #2)
- **Operand kinds:** literal | slot ref | noise source (runtime-sampled, see #3)

Guard ops compile to a comparison opcode (EQ NE LT LE GT GE IS_TRUE ELAPSED_GE ALWAYS) + two operand
IDs; handlers reference an optional guard ID; events reference a handler range + optional timer
(interval) or touch region config.

## Acceptance criteria for the Stage 3 worker

- [ ] Compiles the SAME JSON (existing contract schema, no format change) into the table model once
      at load, then releases the JsonDocument (no retained strings / allocation at exec).
- [ ] All 8 contract points hold in the compiled representation (esp. typed slots, first-match guard
      wins, no backward jumps, slot refs, noise runtime-sampled).
- [ ] `pio run` SUCCESS within SRAM/flash budget (~300KB SRAM, 4MB flash; Stage 2 used 79.4% flash).
- [ ] Three behaviors behave identically to Stage 2 (timer tick/pause, temp rolling graph wobble,
      reaction game phase flow). Confirm on hardware (Brad flashes; Bob cannot).
- [ ] Honest report: what's compile-verified vs what needs eyes on a board.

## Source of truth

- Dispatch Stage 3 against this doc + `docs/plans/2026-10-15-stage2-behavior-store.md` + the contract
  in `docs/GPT-HANDOFF.md`. This doc wins on design specifics.