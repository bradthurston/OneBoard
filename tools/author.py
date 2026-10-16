#!/usr/bin/env python3
"""
OneBoard behavior author — the Stage 4 "canvas leap" tool.

Turns a plain-language request into a valid OneBoard behavior definition, then
(optionally) pushes it to a live board over the wire — NO reflash. The board
compiles the JSON at load; "one board becomes many tools" is literally this.

Usage:
    python3 tools/author.py "a countdown timer showing MM:SS"
    python3 tools/author.py "a stopwatch" --id stopwatch --no-load
    python3 tools/author.py --file request.txt --board 192.168.1.206

Flow:
    1. Build a system prompt from the board's VM vocabulary (embedded below,
       kept in sync with the /schema endpoint + validate_behavior.py).
    2. Ask DeepSeek-V4-Flash (OpenRouter) for the behavior JSON.
    3. Validate with tools/validate_behavior.py (imported Behavior class).
    4. Fix loop: feed validation errors back to the LLM (max --attempts).
    5. Write behaviors/<id>.json.
    6. If --load: point the board's store URL at this machine's serve.py and
       call /load?id=<id> so the board fetches + compiles it right now.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import urllib.request
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, ".."))
BEHAVIORS = os.path.join(REPO, "behaviors")
sys.path.insert(0, HERE)
from validate_behavior import Behavior  # noqa: E402

# Key path is overridable via OPENROUTER_KEY_FILE (portable for public repo);
# defaults to the original location for backwards compat.
KEY_FILE = os.environ.get("OPENROUTER_KEY_FILE",
                          os.path.expanduser("~/.hermes/keys/openrouter"))
MODEL = "deepseek/deepseek-v4-flash-0731"
OPENROUTER = "https://openrouter.ai/api/v1/chat/completions"

# ---------------------------------------------------------------------------
# The board's VM vocabulary — the LLM must write strictly inside this.
# Keep in sync with src/vm.cpp, src/vm.h, tools/validate_behavior.py and the
# board's /schema endpoint. If the vocabulary changes, update ALL of them.
# ---------------------------------------------------------------------------
VOCABULARY = """
OneBoard behavior definitions run on a tiny compiled VM (a CYD ESP32, 240x320
portrait). A behavior JSON has EXACTLY this shape:

{
  "id": "kebab-case-tool-id",            // <= 16 chars, lowercase + dashes
  "name": "Human Readable Name",
  "layout": "fullscreen",
  "state": [ {"name":"slot","type":"int64|float|bool|string","init":<value>}, ... ],
  "events": [ {"name":"tick|touch-down|...", "schedule":{"type":"interval","ms":N}?, "handlers":[...]} ]
}

RULES (hard):
- Max 12 state slots, 6 events, 6 handlers per event, 20 render primitives,
  80 instructions total.
- State slots are TYPED and FIXED: int64, float, bool, string. The VM never
  creates variables; every value references a declared slot.
- Events: "tick" carries schedule {type:interval, ms:N}. Touch is a named
  "touch-down" event with NO schedule. Handlers are FIRST-MATCH-GUARD-WINS —
  order matters; the first handler whose guard matches runs and stops. A
  handler with no guard = always matches.
- A STATIC screen needs NO tick event. The board renders the initial screen on
  load and re-renders only when a handler actually runs. Use a tick ONLY for
  genuinely time-driven screens (count-up timer, temp graph wobble, phase
  machine). A static display (counter, status) renders from the event that
  changes its state.
- Every value ref is {"lit":N} / {"lit":"string"} or {"slot":"slotname"} —
  NEVER a bare value. (Bare values compile to 0/empty silently — this is THE
  recurring bug family.)
- Every screen's render set starts with {"render":{"type":"clear","color":...}}.
- Text NEVER wraps and clips silently past x=240. Font 2 ~= 11px/char. Keep
  labels short. Center text with "center":true and x=120 (font 2) / x=120
  (font 6) — x then means the horizontal center.

VERBS (exactly one key per verb object):
- {"add":{"slot":"s","delta":{"lit":N|"slot":s2|"noise":{"lo":N,"hi":N}}}}
- {"sub":{"slot":"s","delta":...}}            same payload shapes
- {"mul":{"slot":"s","value":{"lit":N|"slot":s2}}}     slot = slot * value
- {"div":{"slot":"s","value":{"lit":N|"slot":s2}}}     slot = slot / value (value 0 -> 0)
- {"mod":{"slot":"s","value":{"lit":N|"slot":s2}}}     slot = slot % value (value 0 -> 0)
- {"clamp":{"slot":"s","min":{"lit":N},"max":{"lit":N}}}  slot = clamp(slot, min, max)
- {"set":{"slot":"s","value":{"lit":N|"slot":s2}}}       (int64/bool)
- {"setf":{"slot":"float_slot","value":...}}  (float slots)
- {"toggle":{"slot":"bool_slot"}}
- {"stamp":{"slot":"s"}}                       slot = now in ms
- {"random":{"slot":"s","min":{"lit":N},"max":{"lit":N}}}  slot = random int in [min,max] inclusive (e.g. dice 1-6)
- {"now_minus":{"slot":"s","from":{"slot":"stamp_slot"}}}  slot = now - from
- {"format":{"slot":"string_slot","op":"ms|temp|react|int","src":{"slot":N}}}
      ms -> "MM:SS"; temp -> "%.1f F"; react -> "%lld ms"; int -> plain integer
- {"progress":{"slot":"width_slot","src":{"slot":N},"max":{"lit":N},"px":{"lit":N}}}
- {"if":{"guard":{...},"then":[...],"else":[...]?}}   bounded forward branch only
- {"render":{...}}                              see render types

GUARD OPS:
- {"eq":{...},"a":{"slot":s},"b":{"lit":N}}  / ne lt le gt ge (compare a vs b)
- {"op":"is_true","a":{"slot":bool_slot}}
- {"op":"elapsed_ge","a":{"slot":stamp_slot},"b":{"lit":N_ms}}  (needs a>0)
- {"op":"always"}
Guard shape: {"op":"eq|ne|lt|le|gt|ge","a":...,"b":...} or {"op":"always"}
or {"op":"is_true","a":...} or {"op":"elapsed_ge","a":...,"b":...}.

RENDER TYPES (fields each accepts):
- clear:      color
- text:       x y src font color center?    src = {"lit":"..."} or {"slot":string_slot}
- rect:       x y w h color
- fillrect:   x y w h color
- line:       x y w h color   (x,y = p1; w,h = p2)
- circle:     x y r color
- bar:        x y w h color src max px      w may be {"slot":n} (progress width)
- graph:      x y w h color slot            slot = float slot to graph (ring buffer)
Fonts 1-7 (font 2 normal, font 6 big). Colors: black white red green blue
yellow cyan magenta orange navy darkgrey darkgray grey gray.

Limits: coords x 0-240, y 0-320; w/h >= 0. Total render primitives <= 20.

Output ONLY the JSON behavior definition, no prose, no markdown fences.
"""


def read_key():
    with open(KEY_FILE) as fh:
        key = fh.read().strip()
    if not key:
        raise SystemExit("OpenRouter key missing at " + KEY_FILE)
    return key


def call_llm(key, messages, max_tokens=3000, temperature=0.3):
    body = json.dumps({
        "model": MODEL,
        "messages": messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
        # Some providers route this model as a reasoning model and burn the
        # whole token budget on invisible `reasoning` output, returning
        # content=None. Disable reasoning so we get the answer directly.
        "reasoning": {"enabled": False},
    }).encode()
    req = urllib.request.Request(
        OPENROUTER, data=body,
        headers={"Authorization": "Bearer " + key,
                 "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=180) as resp:
        data = json.loads(resp.read().decode())
    return data["choices"][0]["message"]["content"]


def extract_json(text):
    """Pull a JSON object out of the LLM reply (tolerates prose/fences)."""
    if not text:
        return None, "empty reply (model returned no content)"
    fence = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.S)
    if fence:
        text = fence.group(1)
    start = text.find("{")
    end = text.rfind("}")
    if start == -1 or end == -1 or end <= start:
        return None, "no JSON object found in reply"
    return text[start:end + 1], None


def validate(doc):
    b = Behavior(doc).run()
    return b.errors, b.warnings


def write_behavior(doc, tool_id):
    os.makedirs(BEHAVIORS, exist_ok=True)
    path = os.path.join(BEHAVIORS, tool_id + ".json")
    with open(path, "w") as fh:
        json.dump(doc, fh, indent=2)
    return path


def http_get(url, timeout=8):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.read().decode()


def http_post(url, data=None, timeout=8):
    body = urllib.parse.urlencode(data or {}).encode()
    req = urllib.request.Request(url, data=body)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read().decode()


def store_reachable(url):
    """True if url responds and looks like the OneBoard behavior store."""
    try:
        txt = http_get(url.rstrip("/") + "/", timeout=4)
        return '"behaviors"' in txt
    except Exception:
        return False


def push_to_board(tool_id, board_ip, store_port):
    """Push behavior to a live board over the wire — no reflash.

    Reuses the board's current store URL when it's already reachable (so the
    tool works regardless of which port serve.py runs on); only repoints it
    when the current store is unreachable.
    """
    base = "http://%s" % board_ip
    store_url = None
    try:
        status = json.loads(http_get(base + "/status", timeout=6))
        cur = status.get("store_url")
        if cur and store_reachable(cur):
            store_url = cur
            print("Store:  %s (reusing board's current store)" % cur)
    except Exception:
        pass

    if store_url is None:
        host_ip = get_lan_ip()
        store_url = "http://%s:%s" % (host_ip, store_port)
        print("Store:  %s" % store_url)
        if not store_reachable(store_url):
            print("WARN: store at %s not reachable from here — is "
                  "store/serve.py running?" % store_url)
        print(http_post(base + "/store", {"url": store_url}))

    print("Board:  %s" % base)
    print(http_post(base + "/load", {"id": tool_id}))
    print(http_get(base + "/status"))


def get_lan_ip():
    """Best-effort LAN IP of this machine (first non-loopback)."""
    s = subprocess.check_output(
        ["hostname", "-I"], text=True, stderr=subprocess.DEVNULL)
    for ip in s.split():
        if ip and not ip.startswith("127."):
            return ip
    raise SystemExit("cannot determine LAN IP — pass --board-store manually?")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("request", nargs="?", help='natural-language request, e.g. "a countdown timer"')
    ap.add_argument("--file", help="read request from file")
    ap.add_argument("--id", help="tool id (kebab-case); default: slug of request")
    ap.add_argument("--board", default=None, help="board IP, e.g. 192.168.1.206")
    ap.add_argument("--store-port", type=int, default=8080,
                    help="port of this machine's serve.py (default 8080)")
    ap.add_argument("--attempts", type=int, default=3,
                    help="max LLM fix-loop attempts (default 3)")
    ap.add_argument("--no-load", action="store_true",
                    help="write the JSON but don't push to the board")
    args = ap.parse_args()

    if args.file:
        with open(args.file) as fh:
            request = fh.read().strip()
    elif args.request:
        request = args.request.strip()
    else:
        ap.error("pass a request string or --file")
    if not request:
        ap.error("empty request")

    tool_id = args.id
    if not tool_id:
        slug = re.sub(r"[^a-z0-9]+", "-", request.lower()).strip("-")
        tool_id = slug[:16] or "tool"

    key = read_key()
    user_prompt = (
        "Write a OneBoard behavior definition for this request:\n\n"
        + request + "\n\n"
        "Tool id must be exactly: " + tool_id + "\n"
        "Respond with ONLY the JSON behavior definition."
    )

    doc = None
    errors = []
    for attempt in range(1, args.attempts + 1):
        print("[attempt %d/%d] asking %s ..." % (attempt, args.attempts, MODEL))
        messages = [{"role": "system", "content": VOCABULARY}]
        if errors:
            messages.append({"role": "user", "content": user_prompt})
            messages.append({"role": "assistant", "content": json.dumps(doc)})
            messages.append({
                "role": "user",
                "content": "Your previous reply FAILED validation. Fix ALL of "
                           "these errors and resend the complete corrected "
                           "JSON:\n" + "\n".join(errors)})
        else:
            messages.append({"role": "user", "content": user_prompt})

        reply = call_llm(key, messages)
        raw, perr = extract_json(reply)
        if perr:
            print("  ! " + perr)
            errors = [perr]
            continue
        try:
            doc = json.loads(raw)
        except json.JSONDecodeError as e:
            print("  ! bad JSON: %s" % e)
            errors = ["invalid JSON: %s" % e]
            continue

        if doc is None:
            errors = ["no JSON produced"]
            continue

        errors, warnings = validate(doc)
        for w in warnings:
            print("  warn: " + w)
        if not errors:
            print("  VALID")
            break
        for e in errors:
            print("  ERROR: " + e)

    if errors:
        print("FAILED after %d attempt(s) — no valid behavior produced." % args.attempts)
        print("The board will NOT be changed. Last draft:")
        if doc:
            print(json.dumps(doc, indent=2))
        return 1

    # id consistency: force the file/register id to the JSON's id if present
    if "id" in doc and doc["id"]:
        tool_id = doc["id"]
    path = write_behavior(doc, tool_id)
    print("Wrote:  %s" % path)

    if not args.no_load:
        if not args.board:
            # Auto-discover: ask the board status endpoint on the common IPs
            raise SystemExit("--board IP required to push; or pass --no-load "
                             "(wrote %s)" % path)
        push_to_board(tool_id, args.board, args.store_port)
    else:
        print("Skipped push (--no-load). Serve it with store/serve.py and "
              "point the board's /store at this machine.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
