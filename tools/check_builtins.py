#!/usr/bin/env python3
"""Extract the PROGMEM behavior literals from src/main.cpp, decode them, and
compare each against its behaviors/<id>.json file.

The firmware's builtins must stay in sync with the store copies: a board flashed
from scratch loads the PROGMEM version, so a fix applied only to behaviors/*.json
does not reach a fresh board.

Usage:
  python3 tools/check_builtins.py            # report drift
  python3 tools/check_builtins.py --write    # rewrite the PROGMEM literals
                                             # from behaviors/*.json
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
MAIN = os.path.join(ROOT, "src", "main.cpp")
BEHAVIORS = os.path.join(ROOT, "behaviors")
BUILTINS = ["timer", "temp-logger", "reaction-game", "tap-counter"]

QUOTED = re.compile(r'"(?:[^"\\]|\\.)*"', re.DOTALL)


def cunescape(s: str) -> str:
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def cescape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def extract(src: str, name: str) -> tuple:
    """Return (json_text, start, end) for `const char <name>[] PROGMEM = ...;`"""
    m = re.search(r"const char %s\[\] PROGMEM =" % re.escape(name), src)
    if not m:
        raise SystemExit(f"PROGMEM literal {name} not found in {MAIN}")
    body_start = m.end()
    end = src.index(";", body_start) + 1
    body = src[body_start:end]
    chunks = QUOTED.findall(body)
    text = "".join(cunescape(c[1:-1]) for c in chunks)
    return text, body_start, end


def literal_for(name: str, json_text: str, indent: str = "  ") -> str:
    """Render a JSON string as a C literal the same way the existing ones are:
    one quoted chunk per line, escaped."""
    # pretty-print so the diff is line-oriented and readable, then chunk it
    pretty = json.dumps(json.loads(json_text), indent=1, ensure_ascii=False)
    lines = pretty.splitlines()
    out = []
    for ln in lines:
        out.append('%s"%s\\n"' % (indent, cescape(ln)))
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true",
                    help="rewrite the PROGMEM literals from behaviors/*.json")
    a = ap.parse_args()

    src = open(MAIN, encoding="utf-8").read()
    drift = []
    for tool in BUILTINS:
        name = {"timer": "TIMER_JSON", "temp-logger": "TEMP_JSON",
                "reaction-game": "REACTION_JSON",
                "tap-counter": "TAP_COUNTER_JSON"}[tool]
        fw_text, _, _ = extract(src, name)
        try:
            fw = json.loads(fw_text)
        except json.JSONDecodeError as e:
            print(f"{tool:15s} PROGMEM JSON DOES NOT DECODE: {e}")
            return 1
        path = os.path.join(BEHAVIORS, tool + ".json")
        store = json.load(open(path)) if os.path.isfile(path) else None
        if store is None:
            print(f"{tool:15s} no store file to compare")
            continue
        if fw == store:
            print(f"{tool:15s} in sync")
            continue
        drift.append(tool)
        print(f"{tool:15s} DRIFT")
        fw_s, st_s = json.dumps(fw, sort_keys=True), json.dumps(store, sort_keys=True)
        if fw_s != st_s:
            # show what differs at the render level, which is what's visible
            def renders(d):
                ops = []

                def walk(actions):
                    for act in actions or []:
                        if "render" in act:
                            ops.append(json.dumps(act["render"], sort_keys=True))
                        if act.get("if"):
                            walk(act["if"].get("then"))
                            walk(act["if"].get("else"))
                for ev in d.get("events", []):
                    for h in ev.get("handlers", []):
                        walk(h.get("run"))
                return ops
            fr, sr = renders(fw), renders(store)
            for op in fr:
                if op not in sr:
                    print(f"    only in firmware: {op}")
            for op in sr:
                if op not in fr:
                    print(f"    only in store:    {op}")
            if fr == sr:
                print("    render ops identical - difference is in state/events")

    if not drift:
        print("\nall builtins in sync")
        return 0

    if not a.write:
        print(f"\n{len(drift)} builtin(s) drifted: {', '.join(drift)}")
        print("run with --write to rewrite them from behaviors/*.json")
        return 1

    print()
    for tool in drift:
        name = {"timer": "TIMER_JSON", "temp-logger": "TEMP_JSON",
                "reaction-game": "REACTION_JSON",
                "tap-counter": "TAP_COUNTER_JSON"}[tool]
        _, start, end = extract(src, name)
        new = literal_for(name, open(os.path.join(BEHAVIORS, tool + ".json")).read())
        # `end` sits just past the declaration's terminating ';', and the slice
        # [start, end) therefore consumes it. Put it back, or the next PROGMEM
        # declaration runs into this one and nothing parses.
        src = src[:start] + "\n" + new + "\n;" + src[end:]
        print(f"rewrote {name} from behaviors/{tool}.json")

    open(MAIN, "w", encoding="utf-8").write(src)

    # verify the rewrite decodes back to the same JSON as the files
    src2 = open(MAIN, encoding="utf-8").read()
    ok = True
    for tool in BUILTINS:
        name = {"timer": "TIMER_JSON", "temp-logger": "TEMP_JSON",
                "reaction-game": "REACTION_JSON",
                "tap-counter": "TAP_COUNTER_JSON"}[tool]
        fw_text, _, _ = extract(src2, name)
        if json.loads(fw_text) != json.load(open(os.path.join(BEHAVIORS, tool + ".json"))):
            print(f"VERIFY FAILED for {tool}")
            ok = False
    print("verify: all builtins decode back to their behaviors/*.json" if ok
          else "verify: FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
