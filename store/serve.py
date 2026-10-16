#!/usr/bin/env python3
"""
OneBoard Behavior Store — dead-simple static JSON host.

Serves the ../behaviors/ directory as individual JSON files.
Usage:
    ./serve.py                    # default port 8080
    ./serve.py --port 9090
    python3 serve.py --port 8080
"""
import http.server
import json
import os
import re
import sys
import argparse
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
BEHAVIORS_DIR = os.path.normpath(os.path.join(HERE, "..", "behaviors"))

# Built into the firmware (src/main.cpp TEMP/TIMER/etc. PROGMEM); deleting their
# store file is meaningless because /load falls back to the firmware copy.
BUILTIN_TOOLS = {"timer", "temp-logger", "reaction-game", "tap-counter"}

# Behavior ids must be kebab-case, no path separators — hard guard against
# deleting arbitrary files via a crafted id (e.g. "../secret.json").
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")


class BehaviorHandler(http.server.SimpleHTTPRequestHandler):
    """Serves .json files from the behaviors directory with CORS headers."""

    def __init__(self, *args, directory=BEHAVIORS_DIR, **kwargs):
        super().__init__(*args, directory=directory, **kwargs)

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS, DELETE")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        super().end_headers()

    def _json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(body)

    def do_DELETE(self):
        # DELETE /delete?id=<tool>  (or DELETE /<tool>.json) — remove a behavior.
        parsed = urllib.parse.urlparse(self.path)
        qs = urllib.parse.parse_qs(parsed.query)
        tool = qs.get("id", [None])[0]
        if not tool:
            m = re.match(r"^/([a-z0-9][a-z0-9-]{0,63})\.json$", parsed.path)
            tool = m.group(1) if m else None
        if not tool:
            self._json(400, {"ok": False, "error": "missing id"})
            return
        if not ID_RE.match(tool):
            self._json(400, {"ok": False, "error": f"invalid id: {tool}"})
            return
        if tool in BUILTIN_TOOLS:
            self._json(409, {"ok": False, "error": f"'{tool}' is a firmware built-in, cannot delete"})
            return
        path = os.path.join(BEHAVIORS_DIR, tool + ".json")
        if not os.path.isfile(path):
            self._json(404, {"ok": False, "error": f"no such tool: {tool}"})
            return
        os.remove(path)
        self._json(200, {"ok": True, "deleted": tool})

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        # Serve individual behavior files
        if self.path.endswith(".json"):
            super().do_GET()
        elif self.path == "/" or self.path == "":
            # List available behaviors
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            behaviors = [f for f in os.listdir(BEHAVIORS_DIR) if f.endswith(".json")]
            listing = {
                "store": "OneBoard Behavior Store",
                "behaviors": sorted(behaviors),
                "count": len(behaviors),
            }
            self.wfile.write(json.dumps(listing, indent=2).encode())
        else:
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"error": "not found"}).encode())


def main():
    parser = argparse.ArgumentParser(description="OneBoard Behavior Store")
    parser.add_argument("--port", "-p", type=int, default=8080, help="Port (default 8080)")
    parser.add_argument("--host", type=str, default="0.0.0.0", help="Host (default 0.0.0.0)")
    args = parser.parse_args()

    server = http.server.HTTPServer((args.host, args.port), BehaviorHandler)
    print(f"OneBoard Behavior Store @ http://{args.host}:{args.port}/")
    print(f"Serving: {BEHAVIORS_DIR}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()


if __name__ == "__main__":
    main()