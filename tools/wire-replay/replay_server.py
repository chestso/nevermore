#!/usr/bin/env python3
"""replay_server.py - HTTP replay of nevermore wire-dump responses.

Turns a nevermore wire dump (docs/WIRE-DEBUG.md; parsed by
wire_dump.py) into pending response ACTIONS, queued per request
signature: method + request target (path [+query]) + exact body.

On each incoming HTTP request the server pops the oldest pending
action whose signature matches and replays what the recording shows
went back on the wire:

  - status line and content type,
  - the SSE event stream (including [DONE]) for streamed exchanges,
  - the whole body for one-shot fetches,
  - the recorded error body for error exchanges,

chunked or close-delimited as recorded. Identical requests in the
dump queue identical actions in recorded-time order; every served
request consumes exactly one action. When no action matches, the
server answers 503 ("not available") with a JSON error and logs a
diagnosis: the expected vs received body (first differing byte), or
the recorded request targets when the path itself is unknown. That
turns scenario divergence into a loud, actionable log line.

Pacing: --pace SECONDS delays each SSE event after the first (and
each slice of whole-body responses), enabling slow-stream scenarios.
Default 0: as fast as the socket allows — order, not timing.

Introspection: GET /__wire_replay__/status returns JSON action counts
(total/served/pending, per signature). Every response carries
X-Wire-Replay-Action / X-Wire-Replay-Status headers.

Limitations (documented, intentional):
  - transport failures (no HTTP head recorded) cannot be reproduced
    over HTTP; the recorded detail is answered as a 502,
  - matching is byte-exact: if the app's request serialization
    changes (tools, headers, ordering), replay diverges — that is
    the signal, the log tells you where,
  - a client that disconnects mid-stream still consumes its action.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Deque, Dict, List, Optional, Sequence, Tuple

import wire_dump
from wire_dump import Dump, Exchange, Response, StreamEvent

# Request-signature type: (method, request target, body).
Signature = Tuple[str, str, str]

# Introspection endpoint (GET, never consumes an action).
STATUS_PATH = "/__wire_replay__/status"

# Whole-body responses are sliced at this size when --pace is active.
BODY_SLICE = 512

LOG_PREFIX = "wire-replay:"


def log(msg: str) -> None:
    print(f"{LOG_PREFIX} {msg}", file=sys.stderr, flush=True)


# ------------------------------------------------------------------ #
# Scenario: pending actions, queued per signature                     #
# ------------------------------------------------------------------ #


@dataclass
class Action:
    """One replayable recorded response, bound to its request."""

    action_id: int
    exchange: Exchange
    signature: Signature
    served: bool = False

    def describe(self) -> str:
        req = self.exchange.request
        resp = self.exchange.response
        kind = resp.kind() if resp else "missing"
        extra = ""
        if resp and resp.stream_events:
            extra = f", {len(resp.stream_events)} SSE event(s)"
        elif resp and resp.body is not None:
            extra = f", body {len(resp.body)}B"
        return f"action #{self.action_id} ({kind}{extra})"


class Scenario:
    """Pending response actions for a set of loaded dumps."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._queues: Dict[Signature, Deque[Action]] = {}
        self._served_count: Dict[Signature, int] = {}
        self._all: List[Action] = []
        # Loose (sequential) mode: serve any pending action without a
        # body match. For rendering repros, where the session content
        # (tool output, AGENTS.md) cannot be reproduced byte-exactly —
        # the request SHAPE and the response stream are what matter.
        self.loose = False

    # -- build ------------------------------------------------------ #

    def build(self, dumps: Sequence[Dump]) -> None:
        """Queue every dump exchange as an action. Within one dump,
        actions are ordered by recorded time (file order breaks ties);
        dumps compose in argument order, so cross-file order is
        explicit rather than a comparison of unrelated clocks."""
        for dump in dumps:
            for ex in sorted(dump.exchanges, key=lambda e: (e.t0, e.index)):
                action = Action(
                    action_id=0, exchange=ex, signature=ex.request.signature()
                )
                self._all.append(action)
        for action_id, action in enumerate(self._all):
            action.action_id = action_id
            self._queues.setdefault(action.signature, deque()).append(action)

    # -- serving ---------------------------------------------------- #

    def take(self, signature: Signature) -> Optional[Action]:
        """Pop the oldest pending action for a signature (None when
        none is queued). In loose mode, a miss falls back to the
        oldest pending action of the same method+path (any body) —
        and only then, when that path is exhausted too, to the oldest
        pending action overall, so a scenario recorded against a
        differently-shaped request still drives the client."""
        with self._lock:
            queue = self._queues.get(signature)
            if queue:
                action = queue.popleft()
            elif self.loose:
                method, path, _ = signature
                candidates = [
                    a for a in self._all if not a.served and a.signature[1] == path
                ]
                if not candidates:
                    candidates = [a for a in self._all if not a.served]
                if not candidates:
                    return None
                action = candidates[0]
                self._queues[action.signature].remove(action)
            else:
                return None
            action.served = True
            self._served_count[signature] = self._served_count.get(signature, 0) + 1
            return action

    # -- introspection ---------------------------------------------- #

    def status(self) -> Dict[str, object]:
        with self._lock:
            served = sum(1 for a in self._all if a.served)
            sigs = []
            for signature in sorted(set(self._queues) | set(self._served_count)):
                pending = len(self._queues.get(signature, ()))
                done = self._served_count.get(signature, 0)
                if pending == 0 and done == 0:
                    continue
                method, path, body = signature
                sigs.append(
                    {
                        "request": f"{method} {path}",
                        "body_bytes": len(body),
                        "body_sha1": _body_digest(body),
                        "pending": pending,
                        "served": done,
                    }
                )
            return {
                "actions_total": len(self._all),
                "actions_served": served,
                "actions_pending": len(self._all) - served,
                "signatures": sigs,
            }

    def miss_reason(self, signature: Signature) -> str:
        """'exhausted' (this exact request had actions, all served) or
        'unmatched' (never recorded)."""
        with self._lock:
            if self._served_count.get(signature, 0) > 0:
                return "exhausted"
            return "unmatched"

    def explain(self, signature: Signature) -> List[str]:
        """Diagnose why no action matched: exhaustion, a body diff on
        the same path, or the recorded request targets."""
        method, path, body = signature
        with self._lock:
            exact_pending = len(self._queues.get(signature, ()))
            exact_served = self._served_count.get(signature, 0)
            same_path = [
                a
                for a in self._all
                if a.signature[0] == method and a.signature[1] == path
            ]
            targets = sorted({(a.signature[0], a.signature[1]) for a in self._all})

        lines: List[str] = []
        if exact_pending:
            lines.append(
                f"{exact_pending} pending action(s) exist but none matched (internal inconsistency)"
            )
            return lines
        if exact_served:
            lines.append(
                f"queue exhausted: {exact_served} action(s) already served for this exact request"
            )
        body_differs = [a for a in same_path if a.signature[2] != body]
        if body_differs:
            lines.append(
                f"{len(body_differs)} recorded action(s) share {method} {path} "
                "but with a different body:"
            )
            for action in body_differs[:3]:
                expected = action.signature[2]
                state = "served" if action.served else "pending"
                lines.append(
                    f"  {action.describe()} [{state}] expected body "
                    f"{len(expected)}B, received {len(body)}B"
                )
                lines.extend("  " + ln for ln in _body_diff(expected, body))
            if len(body_differs) > 3:
                lines.append(f"  ... and {len(body_differs) - 3} more")
        elif not exact_served:
            if len(targets) <= 8:
                lines.append("no recorded exchange for this request; recorded targets:")
                for m, p in targets:
                    lines.append(f"  {m} {p}")
            else:
                lines.append(
                    f"no recorded exchange for this request; {len(targets)} "
                    "distinct targets recorded (--list shows them)"
                )
        return lines


def _body_digest(body: str) -> str:
    """Short stable digest for a request body (status endpoint:
    distinguishes signatures whose method+path are identical)."""
    return hashlib.sha1(body.encode("utf-8", "surrogateescape")).hexdigest()[:12]


def _body_diff(expected: str, received: str) -> List[str]:
    """First-difference context for two near-identical request bodies."""
    limit = min(len(expected), len(received))
    i = 0
    while i < limit and expected[i] == received[i]:
        i += 1
    if i == limit and len(expected) == len(received):
        return ["bodies are byte-identical (signature mismatch elsewhere)"]
    lo = max(0, i - 24)
    return [
        f"first difference at byte {i}:",
        f"    expected: {expected[lo : i + 24]!r}",
        f"    received: {received[lo : i + 24]!r}",
    ]


# ------------------------------------------------------------------ #
# HTTP server                                                         #
# ------------------------------------------------------------------ #


@dataclass
class ServerOptions:
    pace: float = 0.0
    quiet: bool = False


class ReplayHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        server_address,
        handler_cls,
        scenario: Scenario,
        options: ServerOptions,
    ) -> None:
        self.scenario = scenario
        self.options = options
        self._conn_lock = threading.Lock()
        self._conn_seq = 0
        super().__init__(server_address, handler_cls)

    def next_conn_id(self) -> int:
        with self._conn_lock:
            self._conn_seq += 1
            return self._conn_seq


class ReplayHTTPServerV6(ReplayHTTPServer):
    address_family = socket.AF_INET6


def encode_sse_event(event: StreamEvent) -> bytes:
    """Re-encode one recorded SSE event into wire bytes. The dump's
    "message" event label is the parser default and is not emitted
    (the real wire sent a bare `data:`); multi-line data is split
    into one `data:` line per line, per the SSE grammar."""
    parts: List[str] = []
    if event.event and event.event != "message":
        parts.append(f"event: {event.event}\n")
    for line in event.data.split("\n"):
        parts.append(f"data: {line}\n")
    parts.append("\n")
    return "".join(parts).encode("utf-8")


class ReplayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "wire-replay"
    sys_version = ""
    timeout = 30

    # ----- request entry points ------------------------------------ #

    def do_GET(self) -> None:  # noqa: N802 (http.server naming)
        self._replay()

    def do_HEAD(self) -> None:  # noqa: N802
        self._replay(with_body=False)

    def do_POST(self) -> None:  # noqa: N802
        self._replay()

    def do_PUT(self) -> None:  # noqa: N802
        self._replay()

    def do_PATCH(self) -> None:  # noqa: N802
        self._replay()

    def do_DELETE(self) -> None:  # noqa: N802
        self._replay()

    def do_OPTIONS(self) -> None:  # noqa: N802
        self._replay()

    # ----- core ----------------------------------------------------- #

    def _replay(self, with_body: bool = True) -> None:
        conn_id = self.server.next_conn_id()
        raw = self._read_request_body()
        body = raw.decode("utf-8", "surrogateescape")
        method = self.command.upper()
        target = self.path
        signature = (method, target, body)

        if method in ("GET", "HEAD") and target == STATUS_PATH:
            self._serve_status(with_body)
            return

        action = self.server.scenario.take(signature)
        size = len(raw)
        if action is None:
            reason = self.server.scenario.miss_reason(signature)
            self._log(
                f"conn={conn_id} {method} {target} body={size}B -> NO ACTION (503, {reason})"
            )
            for line in self.server.scenario.explain(signature):
                self._log(f"  {line}")
            self._respond_json(
                503,
                {
                    "error": {
                        "message": f"wire-replay: no pending action for {method} {target}",
                        "type": f"replay_{reason}",
                    }
                },
                replay_status=reason,
                with_body=with_body,
            )
            return

        exchange = action.exchange
        response = exchange.response
        if response is None:
            self._log(
                f"conn={conn_id} {method} {target} body={size}B -> {action.describe()}: no recorded response (502)"
            )
            self._respond_json(
                502,
                {"error": {"message": "wire-replay: action has no recorded response"}},
                replay_status="empty",
                with_body=with_body,
            )
            return

        self._log(
            f"conn={conn_id} {method} {target} body={size}B -> {action.describe()}"
        )
        self._serve_recorded(action, response, with_body)

    def _read_request_body(self) -> bytes:
        """Read the request body (Content-Length or chunked)."""
        te = (self.headers.get("Transfer-Encoding") or "").lower()
        if "chunked" in te:
            chunks: List[bytes] = []
            while True:
                line = self.rfile.readline(65536)
                if not line:
                    break
                size_str = line.split(b";", 1)[0].strip()
                try:
                    size = int(size_str, 16)
                except ValueError:
                    break
                if size == 0:
                    while True:  # consume trailers
                        trailer = self.rfile.readline(65536)
                        if trailer in (b"\r\n", b"\n", b""):
                            break
                    break
                chunks.append(self.rfile.read(size))
                self.rfile.read(2)  # CRLF after chunk data
            return b"".join(chunks)
        length = self.headers.get("Content-Length")
        if length:
            try:
                return self.rfile.read(int(length))
            except ValueError:
                return b""
        return b""

    # ----- serving a recorded response ------------------------------ #

    def _serve_recorded(
        self, action: Action, response: Response, with_body: bool
    ) -> None:
        kind = response.kind()
        pace = self.server.options.pace

        if kind == "stream":
            framing = "chunked" if response.chunked else "close"
            content_type = response.content_type or "text/event-stream"
            self._send_head(
                response.status or 200,
                response.status_text,
                content_type,
                framing,
                action_id=action.action_id,
            )
            if not with_body:
                return
            try:
                self._write_events(response.stream_events, framing)
            except OSError as exc:
                self._log(
                    f"  client closed early ({exc.__class__.__name__}); action consumed"
                )
            return

        if kind == "failure":
            detail = (response.error or "recorded transport failure").encode("utf-8")
            self._log(
                "  recorded transport failure cannot be replayed over HTTP; answering 502 with the recorded detail"
            )
            self._send_head(
                502,
                "",
                response.content_type or "text/plain; charset=utf-8",
                "length",
                length=len(detail),
                replay_status="failed",
            )
            if with_body:
                self._write_bytes(detail)
            return

        if kind == "empty":
            self._send_head(
                response.status or 200,
                response.status_text,
                response.content_type or "application/json",
                "length",
                length=0,
                action_id=action.action_id,
            )
            return

        # body / error: whole payload, content type as recorded.
        text = response.body if kind == "body" else (response.error or "")
        data = (text or "").encode("utf-8")
        framing = "chunked" if response.chunked else "length"
        content_type = response.content_type or "application/json"
        self._send_head(
            response.status or 200,
            response.status_text,
            content_type,
            framing,
            length=len(data),
            action_id=action.action_id,
            replay_status="served",
        )
        if not with_body:
            return
        try:
            if framing == "chunked":
                if pace > 0:
                    for i in range(0, len(data), BODY_SLICE):
                        if i > 0:
                            time.sleep(pace)
                        self._write_chunk(data[i : i + BODY_SLICE])
                elif data:
                    self._write_chunk(data)
                self._write_final_chunk()
            else:
                self._write_bytes(data)
        except OSError as exc:
            self._log(
                f"  client closed early ({exc.__class__.__name__}); action consumed"
            )

    def _write_events(self, events: Sequence[StreamEvent], framing: str) -> None:
        pace = self.server.options.pace
        for i, event in enumerate(events):
            if i > 0 and pace > 0:
                time.sleep(pace)
            payload = encode_sse_event(event)
            if framing == "chunked":
                self._write_chunk(payload)
            else:
                self.wfile.write(payload)
                self.wfile.flush()
        if framing == "chunked":
            self._write_final_chunk()

    def _write_bytes(self, data: bytes) -> None:
        pace = self.server.options.pace
        if pace <= 0:
            if data:
                self.wfile.write(data)
                self.wfile.flush()
            return
        for i in range(0, len(data), BODY_SLICE):
            if i > 0:
                time.sleep(pace)
            self.wfile.write(data[i : i + BODY_SLICE])
            self.wfile.flush()

    # ----- response plumbing ---------------------------------------- #

    def _send_head(
        self,
        status: int,
        status_text: str,
        content_type: str,
        framing: str,
        length: int = 0,
        action_id: Optional[int] = None,
        replay_status: str = "served",
    ) -> None:
        self.send_response_only(status, status_text or None)
        self.send_header("Content-Type", content_type)
        if framing == "chunked":
            self.send_header("Transfer-Encoding", "chunked")
        elif framing == "length":
            self.send_header("Content-Length", str(length))
        # "close": no framing header — EOF delimits the body.
        self.send_header("Connection", "close")
        self.send_header("X-Wire-Replay-Status", replay_status)
        if action_id is not None:
            self.send_header("X-Wire-Replay-Action", str(action_id))
        self.end_headers()
        self.close_connection = True

    def _respond_json(
        self, status: int, payload: dict, replay_status: str, with_body: bool = True
    ) -> None:
        data = (json.dumps(payload) + "\n").encode("utf-8")
        self._send_head(
            status,
            "",
            "application/json",
            "length",
            length=len(data),
            replay_status=replay_status,
        )
        if not with_body:
            return
        try:
            self.wfile.write(data)
            self.wfile.flush()
        except OSError:
            pass

    def _serve_status(self, with_body: bool) -> None:
        payload = self.server.scenario.status()
        data = (json.dumps(payload, indent=2) + "\n").encode("utf-8")
        self._send_head(
            200,
            "",
            "application/json",
            "length",
            length=len(data),
            replay_status="status",
        )
        if with_body:
            try:
                self.wfile.write(data)
                self.wfile.flush()
            except OSError:
                pass

    def _write_chunk(self, payload: bytes) -> None:
        self.wfile.write(b"%x\r\n" % len(payload))
        self.wfile.write(payload)
        self.wfile.write(b"\r\n")
        self.wfile.flush()

    def _write_final_chunk(self) -> None:
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    # ----- logging -------------------------------------------------- #

    def _log(self, msg: str) -> None:
        if not self.server.options.quiet:
            log(msg)

    def version_string(self) -> str:
        return self.server_version

    def log_message(self, fmt: str, *args) -> None:  # silence http.server's own log
        pass


# ------------------------------------------------------------------ #
# Startup / CLI                                                       #
# ------------------------------------------------------------------ #


def start_listeners(
    host: str,
    port: int,
    scenario: Scenario,
    options: ServerOptions,
    handler_cls=ReplayHandler,
) -> Optional[List[ReplayHTTPServer]]:
    """Bind one listener per loopback address (localhost -> ::1 and
    127.0.0.1, so the client's resolution order does not matter) or a
    single listener for an explicit host. Returns the servers, or None
    when no bind succeeded."""
    if host in ("", "localhost", None):
        targets = [("::1", ReplayHTTPServerV6), ("127.0.0.1", ReplayHTTPServer)]
    else:
        cls = ReplayHTTPServerV6 if ":" in host else ReplayHTTPServer
        targets = [(host, cls)]

    servers: List[ReplayHTTPServer] = []
    bound_port = port
    for addr, cls in targets:
        try:
            server = cls((addr, bound_port), handler_cls, scenario, options)
        except OSError as exc:
            log(f"bind {addr}:{bound_port} failed: {exc}")
            continue
        bound_port = server.server_address[1]
        servers.append(server)
        if not options.quiet:
            shown = f"[{addr}]" if ":" in addr else addr
            log(f"listening on http://{shown}:{bound_port}")
    return servers or None


def _list_dumps(dumps: Sequence[Dump]) -> None:
    for dump in dumps:
        print(f"{dump.path}: {dump.summary()}")
        for ex in dump.exchanges:
            resp = ex.response
            kind = resp.kind() if resp else "missing"
            status = resp.status if resp else 0
            detail = ""
            if resp is not None and resp.stream_events:
                detail = f"{len(resp.stream_events)} ev"
            elif resp is not None and resp.body is not None:
                detail = f"body {len(resp.body)}B"
            elif resp is not None and resp.error is not None:
                detail = f"error {len(resp.error)}B"
            req = ex.request
            print(
                f"  #{ex.index:<3} conn={ex.conn:<3} xchg={ex.xchg:<2} "
                f"t={ex.t0:9.3f} {status:>3} {kind:<7} {detail:<9} "
                f"{req.method.upper():<4} {req.path} body={len(req.body)}B"
            )
        for warning in dump.warnings:
            print(f"  warning: {warning}")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="wire-replay",
        description="Serve recorded responses from nevermore wire dumps "
        "(docs/WIRE-DEBUG.md). Requests are matched by method + "
        "request target + exact body; each match consumes one queued "
        "action; no match answers HTTP 503.",
    )
    parser.add_argument(
        "dumps",
        nargs="+",
        metavar="DUMP",
        help="wire-debug NDJSON dump file(s); actions queue in argument order",
    )
    parser.add_argument(
        "--host",
        default="localhost",
        help="bind address (default: localhost -> ::1 and 127.0.0.1)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=0,
        help="bind port (default: 0 = ephemeral; the chosen port is printed)",
    )
    parser.add_argument(
        "--pace",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help="delay before each SSE event (after the first) and before "
        "each %dB body slice; 0 = full speed" % BODY_SLICE,
    )
    parser.add_argument(
        "--loose",
        action="store_true",
        help="serve actions sequentially without the byte-exact body match "
        "(for rendering repros whose session content cannot be reproduced); "
        "the request target still wins over an unrelated pending action",
    )
    parser.add_argument(
        "--quiet", action="store_true", help="suppress per-request logging"
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="list the dump's exchanges and exit (no server)",
    )
    parser.add_argument("--version", action="version", version="wire-replay 0.1")
    args = parser.parse_args(argv)

    if args.pace < 0:
        parser.error("--pace must be >= 0")

    dumps: List[Dump] = []
    for path in args.dumps:
        try:
            dump = wire_dump.load(path)
        except OSError as exc:
            log(f"cannot read {path}: {exc}")
            return 2
        dumps.append(dump)
        for warning in dump.warnings:
            log(f"{path}: {warning}")

    if args.list:
        _list_dumps(dumps)
        return 0

    scenario = Scenario()
    scenario.build(dumps)
    scenario.loose = args.loose
    for dump in dumps:
        log(f"loaded {dump.path}: {dump.summary()}")
    if args.loose:
        log("loose mode: body match disabled (sequential actions)")

    options = ServerOptions(pace=args.pace, quiet=args.quiet)
    servers = start_listeners(args.host, args.port, scenario, options)
    if not servers:
        log("no listener could be bound; giving up")
        return 1

    st = scenario.status()
    log(
        f"ready: {st['actions_total']} action(s), "
        f"--pace={args.pace}s, status at http://127.0.0.1:"
        f"{servers[0].server_address[1]}{STATUS_PATH}"
    )

    threads = [
        threading.Thread(
            target=srv.serve_forever, kwargs={"poll_interval": 0.2}, daemon=True
        )
        for srv in servers
    ]
    for thread in threads:
        thread.start()
    try:
        for thread in threads:
            thread.join()
    except KeyboardInterrupt:
        pass
    finally:
        for srv in servers:
            srv.shutdown()
            srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
