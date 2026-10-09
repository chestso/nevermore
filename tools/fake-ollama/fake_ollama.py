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
import base64
import json
import os
import socket
import struct
import sys
import threading
import time
import zlib
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


# A real 64x32 PNG, built in stdlib (a valid container — the manual
# smoke should show an image, not a marker, on a graphics terminal):
# vertical RGB gradient, no interlace.
def make_png(w=64, h=32):
    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8-bit RGB
    rows = b"".join(
        b"\x00"
        + bytes(
            [
                ch
                for x in range(w)
                for ch in (x * 255 // (w - 1), y * 255 // (h - 1), 128)
            ]
        )
        for y in range(h)
    )
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


IMAGEGEN_URL = "data:image/png;base64," + base64.b64encode(make_png()).decode()

# ---- the tool-loop scenario -------------------------------------------
# A scripted TOOL-using turn, so the tool-channel paths can be smoked
# with no model in the loop: the file ledger's pointer body
# (`file-already-read`), a file changed behind the model's back
# (`file-changed`), the model's own write (silent), and the untrusted
# boundary (`web-untrusted`, a real search against the local SearXNG).
# The step is chosen by counting the `tool` messages in the request, so
# it is deterministic and needs no server-side state between requests —
# except the fixture file, which the server owns and rewrites itself.
SMOKE_DIR = "/tmp/nevermore-smoke"
SMOKE_FILE = SMOKE_DIR + "/sample.txt"
SMOKE_V1 = "alpha\nbravo\ncharlie\n"
SMOKE_V2 = "alpha\nBRAVO changed by someone else\ncharlie\n"
SMOKE_EDIT_OLD = "BRAVO changed by someone else"
SMOKE_EDIT_NEW = "bravo edited by the model"
SMOKE_QUERY = "nevermore coding agent"


def write_fixture(text):
    os.makedirs(SMOKE_DIR, exist_ok=True)
    with open(SMOKE_FILE, "w") as f:
        f.write(text)


def tool_call_deltas(name, args, cid):
    """The streamed tool_calls shape: id + name in the first delta, the
    argument JSON split across two more — the accumulation path a real
    provider exercises (a fragment is not valid JSON on its own)."""
    blob = json.dumps(args)
    half = len(blob) // 2
    return [
        {
            "tool_calls": [
                {
                    "index": 0,
                    "id": cid,
                    "type": "function",
                    "function": {"name": name, "arguments": blob[:half]},
                }
            ]
        },
        {
            "tool_calls": [
                {"index": 0, "id": cid, "function": {"arguments": blob[half:]}}
            ]
        },
    ]


# (tool, arguments, note). The note is streamed as the first content
# delta, so the transcript says which step the answer belongs to.
def tool_loop_steps():
    return [
        (
            "read_file",
            {"path": SMOKE_FILE},
            "read_file: the content lands (no note expected)",
        ),
        (
            "read_file",
            {"path": SMOKE_FILE},
            "read_file again: expect the POINTER body + file-already-read",
        ),
        (
            "read_file",
            {"path": SMOKE_FILE},
            "read_file after an EXTERNAL change: expect file-changed",
        ),
        (
            "edit_file",
            {
                "path": SMOKE_FILE,
                "old_string": SMOKE_EDIT_OLD,
                "new_string": SMOKE_EDIT_NEW,
            },
            "edit_file: the model's own write",
        ),
        (
            "read_file",
            {"path": SMOKE_FILE},
            "read_file after the model's OWN write: expect NO note",
        ),
        (
            "web_search",
            {"query": SMOKE_QUERY, "max_results": 3},
            "web_search: expect the web-untrusted block in the result",
        ),
    ]


def tool_loop_deltas(raw):
    """The tool-loop answer for one request: (deltas, finish_reason)."""
    try:
        doc = json.loads(raw)
    except ValueError as e:
        return (
            [{"content": f"[fake-ollama] request body is not JSON: {e}\n\n"}],
            "stop",
        )
    done = sum(1 for m in (doc.get("messages") or []) if m.get("role") == "tool")
    steps = tool_loop_steps()
    if done >= len(steps):
        return (
            [
                {
                    "content": "Tool loop complete: two reads, a change from "
                    "outside, the model's own edit, a re-read, and a "
                    "search. Check the blocks above.\n\n"
                }
            ],
            "stop",
        )
    name, args, note = steps[done]
    if done == 0:
        # A fresh conversation starts from the pristine fixture: the
        # server outlives a chat (and edits it), so a second run would
        # otherwise open on the previous run's content.
        write_fixture(SMOKE_V1)
    if done == 2:
        # The change "somebody else made" lands BEFORE this request is
        # answered, so the read the model is about to run sees it.
        write_fixture(SMOKE_V2)
    deltas = [{"content": f"[fake-ollama] tool-loop step {done + 1}: {note}\n\n"}]
    deltas += tool_call_deltas(name, args, f"call_{done + 1}")
    return (deltas, "tool_calls")


# Phase-sequential: reasoning deltas first (content absent), then
# content. The markdown pieces are split across deltas so the classifier
# sees lines/rows arrive incrementally, as a real stream would.
SCENARIOS = {
    "image": [
        {"content": "I can see the attached image.\n\n"},
        {
            "content": "It is a 64x32 test pattern, and the parts array "
            "arrived intact.\n\n"
        },
    ],
    # The imagegen receive path (docs/OPENROUTER-API.md section 5.1):
    # the WHOLE image as one delta.images event, content "" alongside,
    # then the answer text.
    "imagegen": [
        {
            "content": "",
            "images": [{"type": "image_url", "image_url": {"url": IMAGEGEN_URL}}],
        },
        {"content": "Here is the 64x32 gradient you asked for.\n\n"},
    ],
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
    # Built per request from the tool messages already in the
    # conversation (tool_loop_deltas), not a fixed list.
    "tool-loop": [],
    "prose": [
        {"content": "One line.\n"},
        {"content": "Two line.\n\n"},
        {"content": "- `history.c` is next\n"},
        {"content": "- and `session.c`\n\n"},
        {"content": "Nested **bold with *italic* inside** inline.\n\n"},
        {"content": "Done.\n\n"},
    ],
}


def sse(obj):
    return ("data: " + json.dumps(obj) + "\n\n").encode()


def chunk(delta):
    return {"choices": [{"delta": delta, "index": 0}]}


# Server-side request checks, one per scenario that has a wire contract
# to verify (VISION-PLAN section 2's parts array is the first). A check
# takes the raw request body and returns (problems, summary): an empty
# problems list means the request was shaped the way the scenario
# expects. The verdict rides back as the first content delta, so a
# manual smoke shows it in the transcript itself.
def check_image_request(raw):
    """A user turn carrying an image must send a content-PARTS array:
    the text part first, then one image_url part whose url is a data:
    URI, and never a `detail` field."""
    try:
        doc = json.loads(raw)
    except ValueError as e:
        return ([f"request body is not JSON: {e}"], "")
    msgs = doc.get("messages") or []
    user = [m for m in msgs if m.get("role") == "user"]
    if not user:
        return (["no user message"], "")
    content = user[-1].get("content")
    if not isinstance(content, list):
        return ([f"user content is {type(content).__name__}, not a parts array"], "")
    if not content or content[0].get("type") != "text":
        return (["the text part is not first"], "")
    imgs = [p for p in content if p.get("type") == "image_url"]
    if not imgs:
        return (["no image_url part"], "")
    if "detail" in imgs[0]:
        return (["detail must not be emitted"], "")
    url = (imgs[0].get("image_url") or {}).get("url", "")
    if not url.startswith("data:image/"):
        return ([f"image url is not a data URI: {url[:32]!r}"], "")
    return ([], f"{len(imgs)} image part(s), {len(url)}-byte data URL")


def check_imagegen_request(raw):
    """The editing round-trip (IMAGEGEN): once the model has generated
    an image, the next request replays it on the ASSISTANT message as a
    message-level `images` array of image_url parts, with `content` a
    plain string — never content-parts on an assistant message."""
    try:
        doc = json.loads(raw)
    except ValueError as e:
        return ([f"request body is not JSON: {e}"], "")
    msgs = doc.get("messages") or []
    replayed = 0
    for m in msgs:
        if m.get("role") != "assistant" or "images" not in m:
            continue
        replayed += 1
        if not isinstance(m.get("content"), str):
            return (["assistant image message's content is not a string"], "")
        imgs = m["images"]
        if not isinstance(imgs, list) or not imgs:
            return (["assistant images is not a non-empty array"], "")
        for p in imgs:
            url = (p.get("image_url") or {}).get("url", "")
            if p.get("type") != "image_url" or not url.startswith("data:image/"):
                return (["assistant images carry a non-image_url part"], "")
    return ([], f"{replayed} assistant image message(s) replayed message-level")


SCENARIO_CHECKS = {"image": check_image_request, "imagegen": check_imagegen_request}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    pace = 0.15
    deltas = SCENARIOS["table-fence"]
    scenario = "table-fence"

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
        body = self.rfile.read(clen) if clen else b""
        if not self.path.startswith("/v1/chat/completions"):
            self.send_error(404)
            return
        # A scenario with a wire contract checks the request before it
        # answers, and says so in the stream's first delta.
        deltas = self.deltas
        finish = "stop"
        if self.scenario == "tool-loop":
            deltas, finish = tool_loop_deltas(body)
        check = SCENARIO_CHECKS.get(self.scenario)
        if check:
            problems, summary = check(body)
            if problems:
                note = "[fake-ollama] CHECK FAILED: " + "; ".join(problems)
            else:
                note = "[fake-ollama] check ok: " + summary
            print(f"fake-ollama: {note}", file=sys.stderr, flush=True)
            deltas = [{"content": note + "\n\n"}] + list(deltas)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        for i, d in enumerate(deltas):
            if i:
                time.sleep(self.pace)
            try:
                self.wfile.write(sse(chunk(d)))
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                return
        try:
            self.wfile.write(sse({"choices": [{"delta": {}, "finish_reason": finish}]}))
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
    Handler.scenario = args.scenario

    if args.scenario == "tool-loop":
        # The server owns the fixture and resets it, so a rerun starts
        # from the same content (the previous run edited it).
        write_fixture(SMOKE_V1)

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
        f"scenario={args.scenario}, pace={args.pace}s"
        + (f", fixture={SMOKE_FILE}" if args.scenario == "tool-loop" else ""),
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
