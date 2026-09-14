#!/usr/bin/env python3
"""wire_dump.py - parse nevermore wire-debug NDJSON dumps.

A dump (docs/WIRE-DEBUG.md) is: a plain-text banner (lines starting
with '#'), then one JSON object per line. One exchange is keyed by
(conn, xchg) and its line sequence is exactly:

    request -> response-head -> (stream-event* | response)
                          \\-> error (may follow a head)

This module turns a dump file into an ordered, in-memory form:

  Request   - method/url/body/headers as nevermore constructed them
  Response  - what went back on the wire: a stream of SSE events, a
              whole body, or an error/failure
  Exchange  - request + response + correlation ids + times
  Dump      - the exchange list (file order = time order) + banner

Deliberately forgiving: a truncated last line (a crash during
recording) is tolerated; unknown kinds are ignored (forward
compatibility); old dumps whose request line carried xchg 0 while the
response lines carried 1 still correlate (by conn fallback).
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import urlsplit


@dataclass
class Request:
    """One recorded request, exactly as nevermore constructed it."""

    method: str
    url: str
    body: str
    headers: List[Dict[str, str]] = field(default_factory=list)
    t: float = 0.0

    @property
    def path(self) -> str:
        """Origin-form request target derived from the absolute URL
        (path + query). The host is NOT part of the replay identity:
        the replay server binds loopback and the client's base URL is
        rewritten to the replay address, so only the target and the
        body survive the rewrite."""
        parts = urlsplit(self.url)
        target = parts.path or "/"
        if parts.query:
            target += "?" + parts.query
        return target

    def signature(self) -> Tuple[str, str, str]:
        """The replay matching key: method + request target + exact
        body. Identical requests share a signature; their recorded
        responses queue in time order (one consumed per served
        request)."""
        return (self.method.upper(), self.path, self.body)


@dataclass
class StreamEvent:
    """One complete SSE event as the parser emitted it."""

    event: Optional[str]  # "message" when the dump had no event field
    data: str
    t: float = 0.0


@dataclass
class Response:
    """What actually went back on the wire for an exchange."""

    status: int = 0
    status_text: str = ""
    content_type: str = ""
    chunked: bool = False
    stream_events: List[StreamEvent] = field(default_factory=list)
    body: Optional[str] = None  # whole-body responses (non-streaming)
    error: Optional[str] = None  # error-line detail (body text or reason)
    t: float = 0.0  # response-head time

    def kind(self) -> str:
        """stream | body | error | failure | empty.

        error:   a head status > 0 plus the captured error body text
        failure: no head (status 0), e.g. a transport failure
        empty:   a head with no body/events (e.g. truncated capture)
        """
        if self.stream_events:
            return "stream"
        if self.body is not None:
            return "body"
        if self.error is not None:
            return "error" if self.status else "failure"
        return "empty"


@dataclass
class Exchange:
    conn: int
    xchg: int
    request: Request
    response: Optional[Response] = None
    index: int = 0  # file order (0-based)
    t0: float = 0.0  # request time
    t1: float = 0.0  # last response-line time


@dataclass
class Dump:
    """A parsed dump: the ordered exchange list + metadata."""

    path: str = ""
    banner: List[str] = field(default_factory=list)
    exchanges: List[Exchange] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    provider: Optional[str] = None
    model: Optional[str] = None

    def summary(self) -> str:
        kinds: Dict[str, int] = {}
        for ex in self.exchanges:
            k = ex.response.kind() if ex.response else "missing"
            kinds[k] = kinds.get(k, 0) + 1
        parts = [f"{len(self.exchanges)} exchange(s)"]
        for k in ("stream", "body", "error", "failure", "empty", "missing"):
            if k in kinds:
                parts.append(f"{kinds[k]} {k}")
        return ", ".join(parts)


def _parse_banner_meta(dump: Dump) -> None:
    for line in dump.banner:
        if line.startswith("# provider "):
            rest = line[len("# provider ") :]
            if " model " in rest:
                prov, _, model = rest.partition(" model ")
                dump.provider = prov.strip() or None
                model = model.strip()
                dump.model = model if model and model != "(none)" else None
            else:
                dump.provider = rest.strip() or None
            return


def load(path: str) -> Dump:
    """Parse a wire dump file into a Dump (exchanges in file order)."""
    dump = Dump(path=path)

    with open(path, "r", encoding="utf-8") as f:
        lines = f.read().splitlines()

    data_lines: List[Tuple[int, str]] = []
    for lineno, line in enumerate(lines, start=1):
        if not line:
            continue
        if line.startswith("#"):
            dump.banner.append(line)
            continue
        data_lines.append((lineno, line))
    _parse_banner_meta(dump)

    by_key: Dict[Tuple[int, Any], Exchange] = {}
    current: Dict[int, Exchange] = {}  # most recent request per conn
    order: List[Exchange] = []

    for pos, (lineno, line) in enumerate(data_lines):
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            if pos == len(data_lines) - 1:
                # A crash mid-write leaves one truncated line: expected.
                dump.warnings.append(f"{lineno}: truncated last line skipped")
            else:
                dump.warnings.append(f"{lineno}: unparseable line skipped")
            continue
        if not isinstance(d, dict):
            continue

        kind = d.get("kind")
        conn = d.get("conn")
        if conn is None:
            # Lifecycle-only lines (pre-correlation errors) carry no
            # replayable action.
            continue
        xchg = d.get("xchg")

        if kind == "request":
            ex = Exchange(
                conn=conn,
                xchg=xchg if xchg is not None else 1,
                request=Request(
                    method=d.get("method", "GET"),
                    url=d.get("url", ""),
                    body=d.get("body", "") or "",
                    headers=d.get("headers", []) or [],
                    t=d.get("t", 0.0),
                ),
                index=len(order),
                t0=d.get("t", 0.0),
                t1=d.get("t", 0.0),
            )
            order.append(ex)
            by_key[(conn, ex.xchg)] = ex
            current[conn] = ex
            continue

        # Response-ish lines: exact (conn, xchg) first; else the most
        # recent request on that conn (covers old dumps where the
        # request line's xchg read 0 and the response lines 1).
        ex = by_key.get((conn, xchg)) or current.get(conn)
        if ex is None:
            continue

        if kind == "response-head":
            ex.response = Response(
                status=d.get("status", 0),
                status_text=d.get("statusText", "") or "",
                content_type=d.get("contentType", "") or "",
                chunked=bool(d.get("chunked", False)),
                t=d.get("t", 0.0),
            )
            ex.t1 = d.get("t", ex.t1)
        elif kind == "stream-event":
            if ex.response is None:
                ex.response = Response(t=d.get("t", ex.t1))
            ex.response.stream_events.append(
                StreamEvent(
                    event=d.get("event") or "message",
                    data=d.get("data", "") or "",
                    t=d.get("t", 0.0),
                )
            )
            ex.t1 = d.get("t", ex.t1)
        elif kind == "response":
            if ex.response is None:
                ex.response = Response(t=d.get("t", ex.t1))
            ex.response.body = d.get("body", "") or ""
            ex.t1 = d.get("t", ex.t1)
        elif kind == "error":
            if ex.response is None:
                ex.response = Response(t=d.get("t", ex.t1))
            ex.response.error = d.get("detail", "") or ""
            ex.t1 = d.get("t", ex.t1)
        # connect: ignored (no replayable action).

    dump.exchanges = order
    return dump
