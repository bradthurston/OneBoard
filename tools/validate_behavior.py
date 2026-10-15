#!/usr/bin/env python3
"""OneBoard behavior validator — Stage 4 authoring tool.

Validates a behavior JSON against the exact vocabulary the compiled VM accepts,
so an LLM (or human) can author a behavior and get it checked BEFORE it reaches
the board. Catches the bug family found on hardware 2026-10-15: literal values
wrapped as {"lit":N} that the compiler silently reads as 0/empty when the field
is misspelled or the wrong form is used, unknown verbs/guards/render types/
colors, slot refs to undeclared slots, out-of-bounds coords, etc.

Usage:
    python3 validate_behavior.py behaviors/timer.json
    python3 validate_behavior.py --strict behaviors/reaction-game.json

Exit 0 = valid (or warnings only), 1 = errors found.
"""
import argparse
import json
import sys

# --- Vocabulary (mirrors src/vm.cpp — keep in sync) ---
VERBS = {"add", "sub", "set", "setf", "toggle", "stamp", "now_minus", "random",
         "format", "progress", "render", "if"}
RENDER_TYPES = {"clear", "text", "rect", "fillrect", "line", "circle",
                "bar", "graph"}
GUARD_OPS = {"eq", "ne", "lt", "le", "gt", "ge", "is_true", "elapsed_ge",
             "always"}
FORMAT_OPS = {"ms", "temp", "react", "int"}
SLOT_TYPES = {"int64", "float", "bool", "string"}
COLORS = {"black", "white", "red", "green", "blue", "yellow", "cyan",
          "magenta", "orange", "navy", "darkgrey", "darkgray", "grey", "gray"}
FONT_RANGE = range(1, 8)  # 1..7
COORD_MAX = {"x": 240, "y": 320, "w": 240, "h": 320, "r": 240}
# Render types → fields they accept (only these are read by vmRender/compile)
RENDER_FIELDS = {
    "clear": ["color"],
    "text": ["x", "y", "src", "font", "color", "center"],
    "rect": ["x", "y", "w", "h", "color"],
    "fillrect": ["x", "y", "w", "h", "color"],
    "line": ["x", "y", "w", "h", "color"],          # x,y = p1; w,h = p2
    "circle": ["x", "y", "r", "color"],
    "bar": ["x", "y", "w", "h", "color", "src", "max", "px"],
    "graph": ["x", "y", "w", "h", "color", "slot"],
}


class Behavior:
    def __init__(self, doc, strict=False, fname="<doc>"):
        self.doc = doc
        self.strict = strict
        self.fname = fname
        self.errors = []
        self.warnings = []
        self.slots = {}       # name -> type
        self.events = {}      # name -> event

    # ---------------------------------------------------------------
    def err(self, msg):
        self.errors.append(f"{self.fname}: {msg}")

    def warn(self, msg):
        self.warnings.append(f"{self.fname}: warning: {msg}")

    def run(self):
        d = self.doc
        if not isinstance(d, dict):
            self.err("top level must be a JSON object"); return
        for req in ("id", "name", "state", "events"):
            if req not in d:
                self.err(f"missing required top-level key: '{req}'")
        # state
        if isinstance(d.get("state"), list):
            for s in d["state"]:
                self._check_state(s)
        else:
            self.err("'state' must be an array")
        # events
        if isinstance(d.get("events"), list):
            for ev in d["events"]:
                self._check_event(ev)
        else:
            self.err("'events' must be an array")
        return self

    # ---------------------------------------------------------------
    def _check_state(self, s):
        if not isinstance(s, dict):
            self.err("state entry must be an object"); return
        name = s.get("name")
        if not isinstance(name, str) or not name:
            self.err(f"state entry missing 'name': {s}"); return
        if name in self.slots:
            self.err(f"duplicate slot name '{name}'")
        typ = s.get("type")
        if typ not in SLOT_TYPES:
            self.err(f"slot '{name}': bad type {typ!r} (want {sorted(SLOT_TYPES)})")
        if "init" not in s:
            self.warn(f"slot '{name}' has no 'init'")
        self.slots[name] = typ

    # ---------------------------------------------------------------
    def _check_event(self, ev):
        if not isinstance(ev, dict):
            self.err("event must be an object"); return
        name = ev.get("name")
        if not isinstance(name, str) or not name:
            self.err(f"event missing 'name': {ev}"); return
        if name in self.events:
            self.err(f"duplicate event '{name}'")
        self.events[name] = ev
        # schedule
        sched = ev.get("schedule")
        if sched is not None:
            if not isinstance(sched, dict):
                self.err(f"event '{name}': 'schedule' must be an object")
            elif sched.get("type") not in (None, "interval"):
                self.err(f"event '{name}': unknown schedule type {sched.get('type')!r} (want 'interval')")
            elif sched.get("type") == "interval":
                ms = sched.get("ms")
                if not isinstance(ms, (int, float)) or ms <= 0:
                    self.err(f"event '{name}': interval schedule needs positive 'ms' (got {ms!r})")
        # region (touch) — allowed but the VM dispatches full-screen touch-down
        if "region" in ev and not isinstance(ev["region"], dict):
            self.err(f"event '{name}': 'region' must be an object")
        # handlers
        handlers = ev.get("handlers")
        if not isinstance(handlers, list) or not handlers:
            self.err(f"event '{name}': needs a non-empty 'handlers' array")
            return
        for h in handlers:
            self._check_handler(name, h)

    def _check_handler(self, ev_name, h):
        if not isinstance(h, dict):
            self.err(f"event '{ev_name}': handler must be an object"); return
        if "guard" not in h:
            # guard-less handler == always-match (compileGuard returns G_ALWAYS)
            pass
        else:
            self._check_guard(ev_name, h["guard"])
        if "run" not in h or not isinstance(h["run"], list):
            self.err(f"event '{ev_name}': handler missing 'run' array")
        else:
            for verb in h["run"]:
                self._check_verb(ev_name, verb)
            self._warn_render_order(ev_name, h["run"])

    # ---------------------------------------------------------------
    # Finding-5 guard: render primitives draw at FRAME FLUSH using the state
    # as it is AFTER the whole handler runs, not a snapshot at the render
    # instruction. So a handler that does "set label=A; render(label);
    # set label=B" renders B, not A. Warn when a render reads a slot that a
    # LATER verb in the same block mutates — that's almost always the footgun.
    def _warn_render_order(self, ev_name, run):
        # slots a render reads (text src, coords, bar w, graph slot, color)
        def render_reads(v):
            p = v.get("render", {})
            typ = p.get("type")
            if not isinstance(p, dict) or typ not in RENDER_TYPES:
                return set()
            reads = set()
            for key in ("x", "y", "w", "h", "r"):
                c = p.get(key)
                if isinstance(c, dict) and isinstance(c.get("slot"), str):
                    reads.add(c["slot"])
            if isinstance(p.get("src"), dict) and isinstance(p["src"].get("slot"), str):
                reads.add(p["src"]["slot"])
            if typ == "graph":
                s = p.get("slot")
                if isinstance(s, dict) and isinstance(s.get("slot"), str):
                    reads.add(s["slot"])
            return reads
        # slots a verb writes (its destination)
        def verb_writes(v):
            for k in ("set", "setf", "add", "sub", "toggle", "stamp",
                      "now_minus", "random", "format", "progress"):
                if k in v and isinstance(v[k], dict) and isinstance(v[k].get("slot"), str):
                    return {v[k]["slot"]}
            return set()

        for i, verb in enumerate(run):
            if not isinstance(verb, dict):
                continue
            reads = render_reads(verb)
            if not reads:
                continue
            later_writes = set()
            for later in run[i + 1:]:
                if isinstance(later, dict):
                    later_writes |= verb_writes(later)
            for slot in sorted(reads & later_writes):
                self.warn(
                    f"event '{ev_name}': render reads slot '{slot}' but a later verb "
                    f"in the same handler writes it — renders draw at frame flush, "
                    f"so this draws the LATER value. Move the render after the write "
                    f"if you want the new value, or before it for the old one."
                )

    # ---------------------------------------------------------------
    def _check_guard(self, ev_name, g):
        if not isinstance(g, dict):
            self.err(f"event '{ev_name}': guard must be an object"); return
        op = g.get("op")
        if op not in GUARD_OPS:
            self.err(f"event '{ev_name}': unknown guard op {op!r} (want {sorted(GUARD_OPS)})")
        if op == "always":
            return
        for operand in ("a", "b"):
            if operand not in g:
                if op not in ("is_true",):
                    self.err(f"event '{ev_name}': guard {op} missing operand '{operand}'")
                continue
            self._check_value(g[operand], f"event '{ev_name}' guard.{operand}")

    # ---------------------------------------------------------------
    def _check_verb(self, ev_name, verb):
        if not isinstance(verb, dict):
            self.err(f"event '{ev_name}': verb must be an object"); return
        keys = [k for k in verb if verb.get(k) is not None]
        if len(keys) != 1:
            self.err(f"event '{ev_name}': verb must have exactly one key, got {keys}")
            return
        vname = keys[0]
        if vname not in VERBS:
            self.err(f"event '{ev_name}': unknown verb {vname!r} (want {sorted(VERBS)})")
            return
        p = verb[vname]
        if not isinstance(p, dict):
            self.err(f"event '{ev_name}': verb '{vname}' payload must be an object"); return
        if vname == "render":
            self._check_render(ev_name, p)
        elif vname == "if":
            self._check_if(ev_name, p)
        elif vname == "format":
            self._check_format(ev_name, p)
        elif vname in ("set", "setf"):
            self._check_set(ev_name, vname, p)
        elif vname in ("add", "sub"):
            self._check_addsub(ev_name, vname, p)
        elif vname == "toggle":
            self._check_slot_ref(p.get("slot"), f"event '{ev_name}' toggle.slot")
        elif vname in ("stamp", "now_minus"):
            self._check_stamp(ev_name, vname, p)
        elif vname == "random":
            self._check_random(ev_name, p)
        elif vname == "progress":
            self._check_progress(ev_name, p)

    def _check_if(self, ev_name, p):
        if "guard" not in p:
            self.err(f"event '{ev_name}': if missing 'guard'")
        else:
            self._check_guard(ev_name, p["guard"])
        # then/else must be arrays of verbs
        for kw in ("then", "else"):
            if kw in p:
                if not isinstance(p[kw], list):
                    self.err(f"event '{ev_name}': if.{kw} must be an array")
                else:
                    for v in p[kw]:
                        self._check_verb(ev_name, v)

    def _check_format(self, ev_name, p):
        self._check_slot_ref(p.get("slot"), f"event '{ev_name}' format.slot")
        op = p.get("op")
        if op not in FORMAT_OPS:
            self.err(f"event '{ev_name}': unknown format op {op!r} (want {sorted(FORMAT_OPS)})")
        self._check_value(p.get("src"), f"event '{ev_name}' format.src")

    def _check_set(self, ev_name, vname, p):
        self._check_slot_ref(p.get("slot"), f"event '{ev_name}' {vname}.slot")
        self._check_value(p.get("value"), f"event '{ev_name}' {vname}.value")

    def _check_addsub(self, ev_name, vname, p):
        self._check_slot_ref(p.get("slot"), f"event '{ev_name}' {vname}.slot")
        if "delta" in p:
            delta = p["delta"]
            if not isinstance(delta, dict):
                self.err(f"event '{ev_name}': {vname}.delta must be an object")
                return
            if "noise" in delta:
                nz = delta["noise"]
                if not isinstance(nz, dict) or "lo" not in nz or "hi" not in nz:
                    self.err(f"event '{ev_name}': {vname}.delta.noise needs {{lo,hi}}")
            else:
                self._check_value(delta, f"event '{ev_name}' {vname}.delta")
        else:
            self.err(f"event '{ev_name}': {vname} missing 'delta'")

    def _check_stamp(self, ev_name, vname, p):
        self._check_slot_ref(p.get("slot"), f"event '{ev_name}' {vname}.slot")
        if vname == "now_minus" and "from" in p:
            self._check_value(p["from"], f"event '{ev_name}' now_minus.from")

    def _check_random(self, ev_name, p):
        self._check_slot_ref(p.get("slot"), f"event '{ev_name}' random.slot")
        for k in ("min", "max"):
            if k in p:
                self._check_value(p[k], f"event '{ev_name}' random.{k}")

    def _check_progress(self, ev_name, p):
        self._check_slot_ref(p.get("slot"), f"event '{ev_name}' progress.slot")
        self._check_value(p.get("src"), f"event '{ev_name}' progress.src")
        for k in ("max", "px"):
            if k in p:
                self._check_value(p[k], f"event '{ev_name}' progress.{k}")

    # ---------------------------------------------------------------
    def _check_render(self, ev_name, p):
        typ = p.get("type")
        if typ not in RENDER_TYPES:
            self.err(f"event '{ev_name}': unknown render type {typ!r} (want {sorted(RENDER_TYPES)})")
            return
        allowed = RENDER_FIELDS[typ]
        # unknown keys → likely typo, compiler will silently ignore
        for k in p:
            if k in ("type",):
                continue
            if k not in allowed:
                self.err(f"event '{ev_name}': render '{typ}' field '{k}' not accepted "
                        f"(accepted: {allowed})")
        # coords
        for coord in ("x", "y", "w", "h", "r"):
            if coord in p:
                self._check_coord(p[coord], f"event '{ev_name}' render.{typ}.{coord}", coord)
        # color
        if "color" in p:
            col = p["color"]
            if isinstance(col, dict):
                # {"slot": name} allowed
                if "slot" not in col:
                    self.err(f"event '{ev_name}': render.{typ}.color object must be "
                            f"{{'slot':name}} or a color name string")
            elif not isinstance(col, str) or col not in COLORS:
                self.err(f"event '{ev_name}': render.{typ}.color {col!r} unknown "
                        f"(want {sorted(COLORS)})")
        # font
        if "font" in p:
            f = p["font"]
            val = f.get("lit") if isinstance(f, dict) and "lit" in f else f
            if isinstance(val, str):
                try:
                    val = int(val)
                except ValueError:
                    self.err(f"event '{ev_name}': render.{typ}.font must be a number 1-7, got {val!r}")
            if isinstance(val, (int, float)) and int(val) not in FONT_RANGE:
                self.err(f"event '{ev_name}': render.{typ}.font {val} out of range 1-7")
        # text src (text only)
        if typ == "text":
            if "src" not in p:
                self.err(f"event '{ev_name}': render.text missing 'src'")
            else:
                self._check_value(p["src"], f"event '{ev_name}' render.text.src")
        # graph slot (graph only)
        if typ == "graph":
            if "slot" not in p:
                self.err(f"event '{ev_name}': render.graph missing 'slot' (state slot to graph)")
            else:
                self._check_slot_ref(p["slot"].get("slot") if isinstance(p["slot"], dict) else p["slot"],
                                     f"event '{ev_name}' render.graph.slot")

    # ---------------------------------------------------------------
    def _check_coord(self, v, where, coord):
        val = v.get("lit") if isinstance(v, dict) and "lit" in v else v
        if isinstance(val, str):
            try:
                val = int(val)
            except ValueError:
                self.err(f"{where}: coordinate must be a number or {{'lit':N}}, got {val!r}")
                return
        if isinstance(val, (int, float)):
            iv = int(val)
            if iv < 0 or iv > COORD_MAX[coord]:
                self.err(f"{where}: {iv} out of 0..{COORD_MAX[coord]}")
        elif isinstance(v, dict) and "slot" in v:
            self._check_slot_ref(v["slot"], where)
        else:
            self.err(f"{where}: coordinate must be {{'lit':N}}, {{'slot':name}} or number, got {v!r}")

    def _check_value(self, v, where):
        """A value ref: {"lit":N}, {"lit":"s"}, {"slot":name}, {"delta":..}, {"noise":..} or bare."""
        if v is None:
            self.err(f"{where}: missing value"); return
        if isinstance(v, dict):
            if "slot" in v:
                self._check_slot_ref(v["slot"], where)
            elif "lit" in v:
                pass  # any literal
            elif "noise" in v:
                pass
            elif "delta" in v:
                pass
            else:
                self.err(f"{where}: unrecognised value object {v!r} (want lit/slot/noise/delta)")
        # bare number/string/bool are tolerated by the compiler (lenient), so no error

    def _check_slot_ref(self, ref, where):
        if ref is None:
            return
        if not isinstance(ref, str):
            self.err(f"{where}: slot ref must be a string name, got {ref!r}")
            return
        if ref not in self.slots:
            self.err(f"{where}: slot '{ref}' not declared in state")
        # typed checks (light)
        if ref in self.slots and self.slots[ref] == "string":
            pass

    # ---------------------------------------------------------------
    def summary(self):
        return self.errors, self.warnings


def main():
    ap = argparse.ArgumentParser(description="Validate a OneBoard behavior JSON")
    ap.add_argument("file", help="path to behavior .json")
    ap.add_argument("--strict", action="store_true", help="treat warnings as errors")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    try:
        with open(args.file) as fh:
            doc = json.load(fh)
    except Exception as e:
        print(f"{args.file}: failed to parse JSON: {e}")
        return 1

    b = Behavior(doc, strict=args.strict, fname=args.file).run()
    errors, warnings = b.errors, b.warnings
    if args.json:
        print(json.dumps({"errors": errors, "warnings": warnings,
                          "ok": not errors and not (args.strict and warnings)}))
    else:
        for w in warnings:
            print(w)
        for e in errors:
            print("ERROR:", e)
        if not errors and not warnings:
            print(f"{args.file}: VALID ({len(b.slots)} slots, {len(b.events)} events)")
        elif not errors:
            print(f"{args.file}: valid with {len(warnings)} warning(s)")
        else:
            print(f"{args.file}: {len(errors)} error(s)")
    if errors:
        return 1
    if args.strict and warnings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
