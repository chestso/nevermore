#!/usr/bin/env python3
"""fake-ollama - a stdlib stand-in for the local Ollama daemon.

Serves just enough of the local daemon surface for a manual TUI smoke
of nevermore (which pins `-p ollama:local` to localhost:11434):

    GET  /api/tags             -> a one-model catalog
    POST /v1/chat/completions  -> a paced SSE stream

The stream exercises the whole streaming-transcript IR in one turn:
phase-sequential reasoning (plain, committed before the answer), a
markdown table (block-granular, aligned at commit), a fenced code
block (verbatim), and trailing prose. `--pace` makes the live region
observable mid-stream.

Not wired into `make check` (the suite never hits a network and uses
its own canned servers); this is for the manual/visual pass. See
README.md for the tmux capture recipe.

    tools/fake-ollama/fake_ollama.py [--pace SECONDS] [--port PORT]
        [--host HOST] [--both] [--scenario NAME]

`--both` binds 127.0.0.1 and ::1 in one process (localhost resolves to
::1 first on some boxes; an IPv4-only bind then looks refused).
"""

import argparse
import json
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CATALOG = {
    "models": [
        {
            "name": "fake:1b",
            "model": "fake:1b",
            "modified_at": "2026-01-01T00:00:00Z",
            "size": 1,
            "digest": "0" * 64,
            "details": {
                "family": "fake",
                "parameter_size": "1B",
                "quantization_level": "Q4",
            },
        }
    ]
}

# Phase-sequential: reasoning deltas first (content absent), then
# content. The markdown pieces are split across deltas so the classifier
# sees lines/rows arrive incrementally, as a real stream would.
SCENARIOS = {
    "table-fence": [
        {"reasoning": "Let me line up the regions and check the counts.\n"},
        {"reasoning": "The table needs a numeric column, so 2025 it is.\n"},
        {"content": "Here are the regions:\n\n"},
        {
            "content": "| Region | 2025 |\n"
            "| ------ | ---: |\n"
            "| North  | 1234 |\n"
            "| South  |   56 |\n\n"
        },
        {"content": "And the loader:\n\n"},
        {"content": "```c\nstatic int load(void)\n{\n    return 0;\n}\n```\n\n"},
        {"content": "That is all.\n\n"},
    ],
    "prose": [
        {"content": "One line.\n"},
        {"content": "Two line.\n\n"},
        {"content": "- `history.c` is next\n"},
        {"content": "- and `session.c`\n\n"},
        {"content": "Done.\n\n"},
    ],
}


def sse(obj):
    return ("data: " + json.dumps(obj) + "\n\n").encode()


def chunk(delta):
    return {"choices": [{"delta": delta, "index": 0}]}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    pace = 0.15
    deltas = SCENARIOS["table-fence"]

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/api/tags"):
            body = json.dumps(CATALOG).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)

    def do_POST(self):
        clen = int(self.headers.get("Content-Length", "0") or 0)
        if clen:
            self.rfile.read(clen)
        if not self.path.startswith("/v1/chat/completions"):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        for i, d in enumerate(self.deltas):
            if i:
                time.sleep(self.pace)
            try:
                self.wfile.write(sse(chunk(d)))
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                return
        try:
            self.wfile.write(sse({"choices": [{"delta": {}, "finish_reason": "stop"}]}))
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


class V6Server(ThreadingHTTPServer):
    address_family = socket.AF_INET6


def serve(server):
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--pace",
        type=float,
        default=0.15,
        help="delay before each SSE event after the first",
    )
    ap.add_argument("--port", type=int, default=11434)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument(
        "--both", action="store_true", help="bind 127.0.0.1 and ::1 in one process"
    )
    ap.add_argument("--scenario", choices=sorted(SCENARIOS), default="table-fence")
    args = ap.parse_args()

    Handler.pace = args.pace
    Handler.deltas = SCENARIOS[args.scenario]

    bound = []
    if args.both:
        launchers = [("127.0.0.1", ThreadingHTTPServer), ("::1", V6Server)]
    else:
        launchers = [(args.host, V6Server if ":" in args.host else ThreadingHTTPServer)]

    for host, cls in launchers:
        try:
            srv = cls((host, args.port), Handler)
        except OSError as e:
            print(f"fake-ollama: bind {host}:{args.port} failed: {e}", file=sys.stderr)
            if not bound:
                return 1
            continue
        bound.append(srv)
        serve(srv)

    where = ", ".join(h for h, _ in launchers)
    print(
        f"fake-ollama: {where} port {args.port}, "
        f"scenario={args.scenario}, pace={args.pace}s",
        flush=True,
    )
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
