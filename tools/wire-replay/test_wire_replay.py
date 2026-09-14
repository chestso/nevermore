#!/usr/bin/env python3
"""test_wire_replay.py - tests for the wire-dump replay server.

Run:  python3 tools/wire-replay/test_wire_replay.py
      (or -v for per-test output)

Covers the dump parser (shapes, truncation tolerance, old-dump
correlation fallback), the scenario queues (signature matching,
time ordering, exhaustion diagnostics), and the live HTTP server
(stream/body/error replay, chunked framing bytes, pacing, 503 on
exhaustion/unmatched, the status endpoint).
"""

from __future__ import annotations

import http.client
import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import replay_server  # noqa: E402
import wire_dump  # noqa: E402


# ------------------------------------------------------------------ #
# Fixture helpers                                                     #
# ------------------------------------------------------------------ #

URL = "https://ollama.com:443/v1/chat/completions"


def write_dump(tmpdir: str, name: str, lines: list, banner: bool = True) -> str:
    path = os.path.join(tmpdir, name)
    with open(path, "w", encoding="utf-8") as f:
        if banner:
            f.write("# test dump\n# provider test model test-model\n")
        for line in lines:
            if isinstance(line, str):
                f.write(line + "\n")
            else:
                f.write(json.dumps(line) + "\n")
    return path


def request_line(conn=1, xchg=1, body="{}", url=URL, t=0.0, method="POST"):
    return {
        "t": t,
        "kind": "request",
        "conn": conn,
        "xchg": xchg,
        "method": method,
        "url": url,
        "headers": [],
        "body": body,
    }


def head_line(conn=1, xchg=1, status=200, chunked=True, content_type="text/event-stream", t=0.1):
    return {
        "t": t,
        "kind": "response-head",
        "conn": conn,
        "xchg": xchg,
        "status": status,
        "statusText": "OK" if status == 200 else "",
        "httpVersion": "1.1",
        "contentType": content_type,
        "chunked": chunked,
        "contentLength": -1,
        "headers": [],
    }


def stream_line(conn=1, xchg=1, data="{}", event="message", t=0.2):
    return {
        "t": t,
        "kind": "stream-event",
        "conn": conn,
        "xchg": xchg,
        "event": event,
        "data": data,
    }


def simple_stream_exchange(conn=1, xchg=1, marker="one", url=URL, body="{}", t=0.0):
    """A small streamed exchange: two deltas + [DONE]."""
    return [
        request_line(conn=conn, xchg=xchg, body=body, url=url, t=t),
        head_line(conn=conn, xchg=xchg, t=t + 0.1),
        stream_line(conn=conn, xchg=xchg, data=f'{{"m":"{marker}-a"}}', t=t + 0.2),
        stream_line(conn=conn, xchg=xchg, data=f'{{"m":"{marker}-b"}}', t=t + 0.3),
        stream_line(conn=conn, xchg=xchg, data="[DONE]", t=t + 0.4),
    ]


def decode_sse(payload: bytes) -> list:
    """Decode an SSE byte stream into its data payloads, the way the
    nevermore parser sees them."""
    events, current = [], None
    for line in payload.decode("utf-8").split("\n"):
        if line.startswith("data: "):
            v = line[len("data: ") :]
            current = v if current is None else current + "\n" + v
        elif line.startswith("data:"):
            current = line[len("data:") :]
        elif line == "" and current is not None:
            events.append(current)
            current = None
    return events


class ServerFixture:
    """A running replay server for one set of dumps."""

    def __init__(self, dump_paths, host="127.0.0.1", port=0, pace=0.0, quiet=True):
        dumps = [wire_dump.load(p) for p in dump_paths]
        self.scenario = replay_server.Scenario()
        self.scenario.build(dumps)
        options = replay_server.ServerOptions(pace=pace, quiet=quiet)
        servers = replay_server.start_listeners(host, port, self.scenario, options)
        assert servers, "no listener bound"
        self.servers = servers
        self.port = servers[0].server_address[1]
        self.threads = [
            threading.Thread(
                target=srv.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True
            )
            for srv in servers
        ]
        for thread in self.threads:
            thread.start()

    def close(self):
        for srv in self.servers:
            srv.shutdown()
            srv.server_close()

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        payload = body.encode("utf-8") if isinstance(body, str) else body
        conn.request(method, path, body=payload, headers=headers or {})
        resp = conn.getresponse()
        data = resp.read()
        result = (resp.status, {k.lower(): v for k, v in resp.getheaders()}, data)
        conn.close()
        return result

    def post(self, path, body=""):
        return self.request("POST", path, body)

    def raw(self, request_bytes: bytes) -> bytes:
        s = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        s.sendall(request_bytes)
        chunks = []
        while True:
            b = s.recv(65536)
            if not b:
                break
            chunks.append(b)
        s.close()
        return b"".join(chunks)


# ------------------------------------------------------------------ #
# Parser tests                                                        #
# ------------------------------------------------------------------ #


class TestDumpParser(unittest.TestCase):
    def test_stream_exchange(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_dump(tmp, "a.ndjson", simple_stream_exchange())
            dump = wire_dump.load(path)
        self.assertEqual(len(dump.exchanges), 1)
        ex = dump.exchanges[0]
        self.assertEqual(ex.request.method, "POST")
        self.assertEqual(ex.request.path, "/v1/chat/completions")
        self.assertEqual(ex.response.kind(), "stream")
        self.assertEqual(len(ex.response.stream_events), 3)
        self.assertEqual(ex.response.stream_events[-1].data, "[DONE]")
        self.assertEqual(dump.provider, "test")
        self.assertEqual(dump.model, "test-model")

    def test_body_and_error_exchange(self):
        with tempfile.TemporaryDirectory() as tmp:
            lines = [
                request_line(conn=1, body="q1"),
                head_line(conn=1, status=200, chunked=False, content_type="application/json"),
                {"t": 0.2, "kind": "response", "conn": 1, "xchg": 1, "body": '{"ok":1}'},
                request_line(conn=2, body="q2"),
                head_line(conn=2, status=401, chunked=False, content_type="application/json"),
                {"t": 0.4, "kind": "error", "conn": 2, "xchg": 1, "httpStatus": 401, "stage": "protocol", "detail": "bad key"},
            ]
            path = write_dump(tmp, "b.ndjson", lines)
            dump = wire_dump.load(path)
        self.assertEqual(dump.exchanges[0].response.kind(), "body")
        self.assertEqual(dump.exchanges[0].response.body, '{"ok":1}')
        self.assertEqual(dump.exchanges[1].response.kind(), "error")
        self.assertEqual(dump.exchanges[1].response.error, "bad key")
        self.assertEqual(dump.exchanges[1].response.status, 401)

    def test_truncated_last_line_tolerated(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_dump(tmp, "c.ndjson", simple_stream_exchange())
            with open(path, "a", encoding="utf-8") as f:
                f.write('{"t":9.9,"kind":"stream-event","conn":1,"xchg":1,"da')  # cut mid-line
            dump = wire_dump.load(path)
        self.assertEqual(len(dump.exchanges), 1)
        self.assertTrue(any("truncated" in w for w in dump.warnings))

    def test_old_dump_xchg_fallback(self):
        """Old dumps: request line read xchg 0, response lines xchg 1."""
        with tempfile.TemporaryDirectory() as tmp:
            lines = [
                request_line(conn=1, xchg=0, body="q"),
                head_line(conn=1, xchg=1),
                stream_line(conn=1, xchg=1, data="[DONE]"),
            ]
            path = write_dump(tmp, "d.ndjson", lines)
            dump = wire_dump.load(path)
        self.assertEqual(len(dump.exchanges), 1)
        self.assertEqual(dump.exchanges[0].response.kind(), "stream")

    def test_signature_ignores_host_but_keeps_query(self):
        with tempfile.TemporaryDirectory() as tmp:
            l1 = request_line(conn=1, body="q", url="https://a.example:443/v1/chat/completions?x=1")
            l2 = request_line(conn=2, body="q", url="http://127.0.0.1:9000/v1/chat/completions?x=1")
            l3 = request_line(conn=3, body="q", url="https://a.example:443/v1/chat/completions?x=2")
            path = write_dump(tmp, "e.ndjson", [l1, l2, l3])
            dump = wire_dump.load(path)
        s1 = dump.exchanges[0].request.signature()
        s2 = dump.exchanges[1].request.signature()
        s3 = dump.exchanges[2].request.signature()
        self.assertEqual(s1, s2)  # host + scheme rewritten away
        self.assertNotEqual(s1, s3)  # query is part of the target


# ------------------------------------------------------------------ #
# Scenario tests                                                      #
# ------------------------------------------------------------------ #


class TestScenario(unittest.TestCase):
    def _scenario(self, tmp, lines):
        path = write_dump(tmp, "s.ndjson", lines)
        dump = wire_dump.load(path)
        scenario = replay_server.Scenario()
        scenario.build([dump])
        return scenario

    def test_take_by_signature(self):
        with tempfile.TemporaryDirectory() as tmp:
            scenario = self._scenario(tmp, simple_stream_exchange(marker="one"))
        sig = ("POST", "/v1/chat/completions", "{}")
        action = scenario.take(sig)
        self.assertIsNotNone(action)
        self.assertEqual(action.action_id, 0)
        self.assertIsNone(scenario.take(sig))
        self.assertEqual(scenario.miss_reason(sig), "exhausted")

    def test_duplicate_signatures_queue_in_time_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            lines = simple_stream_exchange(conn=1, marker="first", t=10.0)
            lines += simple_stream_exchange(conn=2, marker="second", t=5.0)
            scenario = self._scenario(tmp, lines)
        sig = ("POST", "/v1/chat/completions", "{}")
        first = scenario.take(sig)
        second = scenario.take(sig)
        # Despite file order (first, second), time order serves the
        # t=5.0 exchange first.
        self.assertEqual(first.exchange.conn, 2)
        self.assertEqual(second.exchange.conn, 1)

    def test_multiple_dumps_compose_in_argument_order(self):
        """Cross-file: argument order wins (each dump's t starts at its
        own zero), so part1's actions serve before part2's."""
        with tempfile.TemporaryDirectory() as tmp:
            p1 = write_dump(tmp, "p1.ndjson", simple_stream_exchange(conn=1, marker="one", t=99.0))
            p2 = write_dump(tmp, "p2.ndjson", simple_stream_exchange(conn=2, marker="two", t=0.0))
            dumps = [wire_dump.load(p1), wire_dump.load(p2)]
        scenario = replay_server.Scenario()
        scenario.build(dumps)
        sig = ("POST", "/v1/chat/completions", "{}")
        self.assertEqual(scenario.take(sig).exchange.conn, 1)
        self.assertEqual(scenario.take(sig).exchange.conn, 2)

    def test_explain_reports_body_diff(self):
        with tempfile.TemporaryDirectory() as tmp:
            scenario = self._scenario(tmp, simple_stream_exchange(body="{}"))
        lines = scenario.explain(("POST", "/v1/chat/completions", '{"x":1}'))
        text = "\n".join(lines)
        self.assertIn("different body", text)
        self.assertIn("first difference at byte", text)

    def test_explain_reports_unknown_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            scenario = self._scenario(tmp, simple_stream_exchange())
        lines = scenario.explain(("GET", "/nope", ""))
        text = "\n".join(lines)
        self.assertIn("no recorded exchange", text)
        self.assertIn("POST /v1/chat/completions", text)

    def test_status_counts(self):
        with tempfile.TemporaryDirectory() as tmp:
            scenario = self._scenario(tmp, simple_stream_exchange())
        scenario.take(("POST", "/v1/chat/completions", "{}"))
        st = scenario.status()
        self.assertEqual(st["actions_total"], 1)
        self.assertEqual(st["actions_served"], 1)
        self.assertEqual(st["actions_pending"], 0)


# ------------------------------------------------------------------ #
# Server tests                                                        #
# ------------------------------------------------------------------ #


class TestServer(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.fixtures = []

    def fixture(self, lines, **kwargs):
        path = write_dump(self._tmp.name, f"dump{len(self.fixtures)}.ndjson", lines)
        fx = ServerFixture([path], **kwargs)
        self.addCleanup(fx.close)
        self.fixtures.append(fx)
        return fx

    def test_serves_recorded_stream(self):
        fx = self.fixture(simple_stream_exchange(marker="one"))
        status, headers, data = fx.post("/v1/chat/completions", "{}")
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "text/event-stream")
        self.assertEqual(headers["x-wire-replay-status"], "served")
        self.assertEqual(headers["x-wire-replay-action"], "0")
        self.assertEqual(decode_sse(data), ['{"m":"one-a"}', '{"m":"one-b"}', "[DONE]"])

    def test_duplicate_signatures_serve_in_order(self):
        lines = simple_stream_exchange(conn=1, marker="first", t=10.0)
        lines += simple_stream_exchange(conn=2, marker="second", t=5.0)
        fx = self.fixture(lines)
        _, _, first = fx.post("/v1/chat/completions", "{}")
        _, _, second = fx.post("/v1/chat/completions", "{}")
        self.assertEqual(decode_sse(first)[0], '{"m":"second-a"}')
        self.assertEqual(decode_sse(second)[0], '{"m":"first-a"}')

    def test_exhaustion_is_503(self):
        fx = self.fixture(simple_stream_exchange())
        status, _, _ = fx.post("/v1/chat/completions", "{}")
        self.assertEqual(status, 200)
        status, headers, data = fx.post("/v1/chat/completions", "{}")
        self.assertEqual(status, 503)
        self.assertEqual(headers["x-wire-replay-status"], "exhausted")
        payload = json.loads(data)
        self.assertEqual(payload["error"]["type"], "replay_exhausted")

    def test_unmatched_is_503(self):
        fx = self.fixture(simple_stream_exchange())
        status, headers, data = fx.post("/v1/other", "")
        self.assertEqual(status, 503)
        self.assertEqual(headers["x-wire-replay-status"], "unmatched")
        payload = json.loads(data)
        self.assertEqual(payload["error"]["type"], "replay_unmatched")

    def test_body_response(self):
        lines = [
            request_line(conn=1, body="", url="https://ollama.com:443/api/tags", method="GET"),
            head_line(conn=1, status=200, chunked=False, content_type="application/json"),
            {"t": 0.2, "kind": "response", "conn": 1, "xchg": 1, "body": '{"models":[]}'},
        ]
        fx = self.fixture(lines)
        status, headers, data = fx.request("GET", "/api/tags")
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-length"], str(len(b'{"models":[]}')))
        self.assertEqual(data, b'{"models":[]}')

    def test_error_exchange_replays_status_and_body(self):
        lines = [
            request_line(conn=1, body="q"),
            head_line(conn=1, status=401, chunked=False, content_type="application/json"),
            {"t": 0.2, "kind": "error", "conn": 1, "xchg": 1, "httpStatus": 401,
             "stage": "protocol", "detail": "invalid api key"},
        ]
        fx = self.fixture(lines)
        status, headers, data = fx.post("/v1/chat/completions", "q")
        self.assertEqual(status, 401)
        self.assertEqual(data, b"invalid api key")

    def test_chunked_framing_bytes(self):
        fx = self.fixture(simple_stream_exchange(marker="one"))
        body = b"{}"
        raw = fx.raw(
            b"POST /v1/chat/completions HTTP/1.1\r\n"
            b"Host: 127.0.0.1\r\n"
            b"Content-Type: application/json\r\n"
            b"Content-Length: " + str(len(body)).encode() + b"\r\n"
            b"Connection: close\r\n\r\n" + body
        )
        head, _, rest = raw.partition(b"\r\n\r\n")
        self.assertIn(b"Transfer-Encoding: chunked", head)
        self.assertTrue(rest.endswith(b"0\r\n\r\n"), "chunked terminator missing")
        # A chunk-size line precedes the first SSE event.
        self.assertRegex(rest[:16], rb"^[0-9a-f]+\r\n")
        self.assertIn(b"data: [DONE]\n", rest)
        # The framing survives a real SSE decode.
        self.assertEqual(decode_sse(rest), ['{"m":"one-a"}', '{"m":"one-b"}', "[DONE]"])

    def test_pace_slows_stream(self):
        lines = [
            request_line(conn=1, body="{}"),
            head_line(conn=1),
            stream_line(conn=1, data='"a"', t=0.2),
            stream_line(conn=1, data='"b"', t=0.3),
            stream_line(conn=1, data="[DONE]", t=0.4),
        ]
        fx = self.fixture(lines, pace=0.06)
        t0 = time.monotonic()
        status, _, data = fx.post("/v1/chat/completions", "{}")
        elapsed = time.monotonic() - t0
        self.assertEqual(status, 200)
        self.assertEqual(decode_sse(data)[-1], "[DONE]")
        # Two inter-event delays (after the first event).
        self.assertGreaterEqual(elapsed, 0.10)
        self.assertLess(elapsed, 5.0)

    def test_fast_default_is_fast(self):
        fx = self.fixture(simple_stream_exchange())
        t0 = time.monotonic()
        status, _, _ = fx.post("/v1/chat/completions", "{}")
        elapsed = time.monotonic() - t0
        self.assertEqual(status, 200)
        self.assertLess(elapsed, 0.5)

    def test_status_endpoint(self):
        fx = self.fixture(simple_stream_exchange())
        fx.post("/v1/chat/completions", "{}")
        status, headers, data = fx.request("GET", "/__wire_replay__/status")
        self.assertEqual(status, 200)
        st = json.loads(data)
        self.assertEqual(st["actions_total"], 1)
        self.assertEqual(st["actions_served"], 1)
        self.assertEqual(st["actions_pending"], 0)
        self.assertEqual(st["signatures"][0]["served"], 1)
        self.assertEqual(st["signatures"][0]["body_bytes"], 2)
        self.assertRegex(st["signatures"][0]["body_sha1"], r"^[0-9a-f]{12}$")

    def test_event_label_re_encoded(self):
        """A non-default event: field round-trips; "message" does not."""
        lines = [
            request_line(conn=1, body="{}"),
            head_line(conn=1),
            stream_line(conn=1, event="ping", data="pong", t=0.2),
            stream_line(conn=1, data="[DONE]", t=0.3),
        ]
        fx = self.fixture(lines)
        _, _, data = fx.post("/v1/chat/completions", "{}")
        self.assertIn(b"event: ping\ndata: pong\n\n", data)

    def test_multiline_data_re_split(self):
        lines = [
            request_line(conn=1, body="{}"),
            head_line(conn=1),
            stream_line(conn=1, data="line1\nline2", t=0.2),
            stream_line(conn=1, data="[DONE]", t=0.3),
        ]
        fx = self.fixture(lines)
        _, _, data = fx.post("/v1/chat/completions", "{}")
        self.assertEqual(decode_sse(data), ["line1\nline2", "[DONE]"])

    def test_stream_without_chunked_flag_is_close_delimited(self):
        lines = simple_stream_exchange()
        lines[1]["chunked"] = False
        fx = self.fixture(lines)
        status, headers, data = fx.post("/v1/chat/completions", "{}")
        self.assertEqual(status, 200)
        self.assertNotIn("transfer-encoding", headers)
        self.assertEqual(decode_sse(data)[-1], "[DONE]")


if __name__ == "__main__":
    unittest.main(verbosity=2)