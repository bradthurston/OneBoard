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
import sys
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))
BEHAVIORS_DIR = os.path.normpath(os.path.join(HERE, "..", "behaviors"))


class BehaviorHandler(http.server.SimpleHTTPRequestHandler):
    """Serves .json files from the behaviors directory with CORS headers."""

    def __init__(self, *args, directory=BEHAVIORS_DIR, **kwargs):
        super().__init__(*args, directory=directory, **kwargs)

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        super().end_headers()

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