# wire-replay — replay nevermore wire dumps

A dependency-free Python HTTP server that reads a nevermore wire-debug
dump (`NEVERMORE_DEBUG_WIRE=1`; format in `docs/WIRE-DEBUG.md`) and
replays the recorded responses against live requests. It exists to make
nevermore debugging scenarios reproducible: capture once, replay as
many times as you want, deterministically.

```
tools/wire-replay/wire-replay DUMP [DUMP ...] [--port PORT] [--pace S]
```

## How it works

The dump is turned into **pending response actions** — one per recorded
exchange, each holding exactly what went back on the wire: status,
content type, the SSE event stream (including `[DONE]`), the whole body,
or the recorded error body. Actions are matched to incoming requests by
signature:

> **method + request target (path [+query]) + exact body**

The URL's _host_ is deliberately not part of the signature: the replay
server binds loopback, so whatever base URL points nevermore at it, the
host necessarily differs from the recorded one. The path, query and
body (the parts the client actually constructs) are matched byte-exact.

Every served request consumes exactly one action. When the dump holds
several identical requests, their actions queue in recorded-time order
and are served in that order; when several dumps are given, they
compose in argument order. When no action matches, the server answers
**HTTP 503 "not available"** (`{"error": {"type": "replay_exhausted" |
"replay_unmatched"}}`) — and logs a diagnosis, so a diverged scenario
tells you _where_ it diverged:

```
wire-replay: conn=2 POST /v1/chat/completions body=3257B -> NO ACTION (503, unmatched)
wire-replay:   4 recorded action(s) share POST /v1/chat/completions but with a different body:
wire-replay:     action #0 (stream, 4 SSE event(s)) [pending] expected body 3256B, received 3257B
wire-replay:     first difference at byte 10:
wire-replay:         expected: '{"model":"minimax-m3","messages":['
wire-replay:         received: '{"model":"gpt-oss:20b","messages":'
```

## Usage

```sh
# serve a dump on loopback, full speed (order matters, timing does not)
tools/wire-replay/wire-replay ~/.local/state/nevermore/wire/nevermore-wire-*.ndjson --port 11434

# slow stream: delay before each SSE event (after the first)
tools/wire-replay/wire-replay DUMP --pace 0.05

# inspect a dump without serving it
tools/wire-replay/wire-replay DUMP --list

# several dumps queue in argument order (compose scenarios)
tools/wire-replay/wire-replay part1.ndjson part2.ndjson
```

Options:

| Flag          | Meaning                                                                                   |
| ------------- | ----------------------------------------------------------------------------------------- |
| `--host HOST` | bind address; default `localhost` binds `::1` **and** `127.0.0.1`                         |
| `--port PORT` | bind port; default `0` = ephemeral (the chosen port is printed)                           |
| `--pace S`    | delay before each SSE event after the first (and each 512 B body slice); `0` = full speed |
| `--loose`     | serve actions in recorded order **without** the byte-exact body match (see below)         |
| `--quiet`     | suppress per-request logging                                                              |
| `--list`      | list the dump's exchanges and exit                                                        |

**`--loose` is for rendering repros, not wire-accuracy work.** A dump
whose later rounds embed tool output (a directory listing, `ps` output,
a file's contents) only matches byte-exact if the client reproduces
that exact session content — impossible after the fact. Loose mode
serves the oldest pending action for the request target (falling back
to the oldest pending action overall), so a captured conversation still
drives the client end-to-end. The request shape and the response stream
are faithful; the body match verdict is deliberately waived, and the
per-request log marks the substitution. Never use it to judge whether
nevermore constructs a correct request.

Introspection: `GET /__wire_replay__/status` returns action counts
(total / served / pending) plus per-signature counts with body sizes
and short SHA-1 digests (distinguishing same-path requests), and the
current dump paths under `dumps`. Every response carries
`X-Wire-Replay-Action` and `X-Wire-Replay-Status` headers.

## Rearm: another run without a restart

A replay scenario is single-shot: every served request consumes one
action, and once the queues are empty further requests 503. Instead
of killing the server (and losing the port, the terminal scrollback,
the `--pace`/`--loose` flags), re-arm it:

```sh
curl -X POST http://127.0.0.1:11434/__wire_replay__/rearm
```

That reloads the dump file(s) from disk and resets every queue — the
next client run replays the same scenario against the same port. A
bare rearm also re-reads the files, so an edited or replaced dump at
the same path is picked up (trim a dump to the interesting rounds,
rearm, rerun).

`?dumps=PATH[,PATH...]` swaps the scenario to a different dump set
without restarting:

```sh
curl -X POST 'http://127.0.0.1:11434/__wire_replay__/rearm?dumps=/path/other.ndjson'
```

A failed rearm (unreadable path) answers 500 and leaves the previous
queues intact, so a typo never wedges the server. Only POST rearms —
a GET on the path is just another unmatched request.

## Typical workflow

```sh
# 1. capture a real session
NEVERMORE_DEBUG_WIRE=1 nevermore ...          # writes ~/.local/state/nevermore/wire/*.ndjson

# 2. replay it
tools/wire-replay/wire-replay DUMP --port 11434 --pace 0.02

# 3. point nevermore at the replay server and reproduce the scenario
#    (the same model id as the dump, or requests won't match — the log
#    tells you when they don't)
```

`--port 11434` maps onto nevermore's default local-Ollama endpoint; a
dump recorded against Ollama Cloud (`https://ollama.com/v1`) is matched
the same way — the host rewrite is what makes cloud dumps replayable
locally.

### Driving nevermore at the replay server

The `test:replay` provider exists for exactly this: a loopback base, no
auth, a tokenless static catalog, and the same OpenAI-compatible wire
the other providers use. Two knobs make a recorded dump reproducible:

```sh
# 1. capture a real session (any provider)
NEVERMORE_DEBUG_WIRE=1 nevermore -p opencode:go -m deepseek-v4.1-flash ...

# 2. serve the dump (--loose: later rounds embed unreproducible tool
#    output; see the flag note above)
tools/wire-replay/wire-replay ~/.local/state/nevermore/wire/DUMP.ndjson \
    --port 11434 --loose &

# 3. replay it through the real client. NEVERMORE_BASE_URL overrides the
#    provider's endpoint, and its PATH is what the dump's request target
#    was recorded under (a Go dump carries /zen/go/v1).
NEVERMORE_BASE_URL=http://localhost:11434/zen/go/v1 \
NEVERMORE_AUTHINFO=/dev/null NM_NO_LIVE_CATALOG=1 \
    nevermore -p test:replay -m deepseek-v4.1-flash
```

`NM_NO_LIVE_CATALOG=1` keeps the model popup from fetching `/models`
over the wire (the replay server holds only chat completions).
`NEVERMORE_AUTHINFO=/dev/null` is belt-and-braces: `test:replay` is
keyless on loopback, but a stray real authinfo entry would otherwise
ride along.

## Behavior notes

- **Order, not timing.** By default responses stream as fast as the
  socket allows; recorded inter-event gaps are ignored. `--pace` is the
  slow-stream switch.
- **Byte-exact matching.** If nevermore's request serialization changes
  (different tool schemas, system prompt, message assembly), replay
  stops matching — that divergence is the signal, and the byte-offset
  diff points at it. Dumps are only valid for the app revision (and
  session content) that produced them.
- **A disconnected client still consumes its action** (each request
  pops exactly one action, whether or not the response was read).
- **Transport failures** (dump lines with no HTTP head, e.g. connect
  refused) cannot be reproduced over HTTP; the recorded detail is
  answered as a 502 and noted in the log.
- **Chunked framing** is re-encoded when the recording shows it, with
  `0\r\n\r\n` termination; `event:` labels are re-emitted only for
  non-default events; multi-line `data` is re-split per the SSE grammar.

## Files

| File                  | Purpose                              |
| --------------------- | ------------------------------------ |
| `wire-replay`         | CLI entry point                      |
| `replay_server.py`    | scenario queues + HTTP server        |
| `wire_dump.py`        | wire-debug NDJSON dump parser        |
| `test_wire_replay.py` | tests: `python3 test_wire_replay.py` |

## Tests

```sh
python3 tools/wire-replay/test_wire_replay.py
```

Covers the parser (shapes, truncation tolerance, old-dump correlation),
the queues (signature matching, time ordering, exhaustion diagnostics),
and the live server (stream/body/error replay, chunked framing bytes,
pacing, 503s, status endpoint). Pure stdlib — no prerequisites.
