# fake-ollama — a daemon stand-in for the TUI smoke

A dependency-free Python HTTP server that speaks just enough of the
local Ollama surface for nevermore's manual smoke:

- `GET /api/tags` — a one-model catalog
- `POST /v1/chat/completions` — a paced SSE stream

It exists because a real local daemon is not available on every dev
box, and a manual TUI smoke should not depend on one. `-p ollama:local`
pins nevermore's base URL to `http://localhost:11434/v1`, so the fake
server binds that port and the unmodified binary talks to it.

`make check` never touches this (the suite uses its own canned
loopback servers and never hits a network); this is for the
manual/visual pass.

## Usage

```sh
# serve the default table-fence scenario on 127.0.0.1 and ::1
tools/fake-ollama/fake_ollama.py --both

# slow it down so the live region is observable mid-stream
tools/fake-ollama/fake_ollama.py --both --pace 0.6

# other scenario / port
tools/fake-ollama/fake_ollama.py --scenario prose --port 11434
```

| Flag          | Meaning                                                      |
| ------------- | ------------------------------------------------------------ |
| `--pace S`    | delay before each SSE event after the first (0 = full speed) |
| `--port PORT` | bind port; default 11434 (nevermore's local-Ollama default)  |
| `--host HOST` | bind address; default 127.0.0.1                              |
| `--both`      | bind 127.0.0.1 **and** ::1 in one process                    |
| `--scenario`  | `table-fence` (default), `prose`, `image`, or `imagegen`         |

**Bind both stacks.** `localhost` resolves to `::1` first on some
boxes; an IPv4-only bind then shows a "connection refused" that looks
like an app bug. `--both` avoids the whole class.

## The scenarios

- **`table-fence`** — phase-sequential reasoning, a markdown table, a
  fenced code block, trailing prose. Exercises the classifier, the
  plain renderer, block-granular table commit, and stream phase order
  in one turn.
- **`prose`** — paragraphs and a list, for line-granular lookahead and
  loose-list behavior.
- **`image`** — the vision smoke: a scenario with a wire CONTRACT, so
  the server checks the request before answering. A user turn carrying
  an image must send a content-parts array (text part first, then one
  `image_url` part whose url is a `data:` URI, never `detail`); the
  verdict is printed to the server's stderr and streamed back as the
  first content delta, so the transcript shows whether the parts array
  arrived intact. Pairs with the TUI's `/img` (or `-i` in ask mode).
- **`tool-loop`** — the TOOL-channel smoke, with no model in the loop.
  A scripted tool sequence driven by the number of `tool` messages
  already in the request, so each step is deterministic: `read_file`
  the fixture (the content lands), `read_file` it again (the file
  ledger's POINTER body + the `file-already-read` block), `read_file`
  after the SERVER rewrites the fixture behind the model's back
  (`file-changed`), `edit_file` (the model's own write), `read_file`
  again (NO note: the model's own write silences the ledger), then a
  real `web_search` against the local SearXNG (the `web-untrusted`
  block). The fixture is `/tmp/nevermore-smoke/sample.txt`, reset by
  the server at the start of every conversation, so a rerun is
  repeatable. This is the scenario that exercises the reminder
  plumbing (block placement, the purple role, the panel/wire byte
  equality) without a provider.
- **`imagegen`** — the image-GENERATION smoke (the receive direction):
  the stream's first event is one `delta.images` chunk carrying a real
  64x32 PNG (built in stdlib) as a `data:` URL, then the answer text.
  Its contract is the editing round-trip: a later request must replay
  the assistant message with a MESSAGE-LEVEL `images` array and a plain
  string `content` (never content-parts on an assistant message). The
  TUI shows the image through the one IMAGE-block pipeline (the marker
  under tmux, the picture on a kitty terminal); ask mode saves
  `nevermore-image-1.png` and keeps stdout clean.

Deltas are split so markdown arrives incrementally (lines/rows land
across separate SSE events), as a real stream would.

## The smoke recipe (tmux, not a raw pty)

```sh
tools/fake-ollama/fake_ollama.py --both --pace 0.6 &
tmux new-session -d -s smoke -x 90 -y 28
tmux send-keys -t smoke \
    "NEVERMORE_AUTHINFO=/dev/null ./build/src/nevermore -p ollama:local" Enter
sleep 2
tmux send-keys -t smoke "show me the regions and a loader" Enter
# mid-stream: the live region (tail + spinner)
tmux capture-pane -t smoke -p
sleep 6
tmux capture-pane -t smoke -p -S -40        # the committed scrollback
tmux send-keys -t smoke "/quit" Enter
tmux kill-session -t smoke
```

Use **tmux `capture-pane`**, never a raw pty capture: the pty stream
mixes frame repaints with scrollback and shows phantom duplicates that
are not on the real screen. `capture-pane` renders through a true
terminal emulator and is the screen truth.

What to check: no staircase (bare LF), no duplicated lines, no stale
spinner rows, reasoning before the answer, the table box aligned, the
fence verbatim.

The vision pass reuses the same recipe with the image scenario:

```sh
tools/fake-ollama/fake_ollama.py --both --scenario image --pace 0.6 &
# ... start the TUI as above, then:
tmux send-keys -t smoke "/img /path/to/pic.png" Enter
tmux send-keys -t smoke "what is in this image?" Enter
sleep 4
tmux capture-pane -t smoke -p -S -40
```

The answer's first line is the server's verdict (`[fake-ollama] check
ok: 1 image part(s), ...`), the echo above it is the IMAGE marker (the
tmux pane is not a graphics terminal) or the image itself on a kitty
terminal, and the resize/scrollback behavior is the same ladder IR
step 5 already covers.

## The tool-loop pass (the reminder + file-ledger smoke)

No model, no key: the scenario scripts the tool calls, so every
reminder the harness fires is reproducible. Ask mode is the fastest
read (stdout is the answer, stderr the tool activity):

```sh
tools/fake-ollama/fake_ollama.py --both --scenario tool-loop &
NEVERMORE_AUTHINFO=/dev/null ./build/src/nevermore \
    -p ollama:local -m fake:1b "go"
```

What each step must show (the step note is streamed as the assistant's
first content delta, so the transcript says which is which):

| Step | Call           | Expected                                                          |
| ---- | -------------- | ----------------------------------------------------------------- |
| 1    | `read_file`    | the fixture's content, no reminder                                 |
| 2    | `read_file`    | the POINTER body + `file-already-read`                             |
| 3    | `read_file`    | the CURRENT content + `file-changed` (the server rewrote it)       |
| 4    | `edit_file`    | the edit result, no reminder                                       |
| 5    | `read_file`    | the content, NO note (the model's own write silences the ledger)   |
| 6    | `web_search`   | real SearXNG results + `web-untrusted`                             |

For the panel (placement + the purple `NM_SGR_REMINDER` role) drive the
same scenario through the TUI and read the bytes, not the pixels:

```sh
tmux new-session -d -s smoke -x 90 -y 28
tmux send-keys -t smoke "./build/src/nevermore -p ollama:local -m fake:1b" Enter
sleep 2
tmux send-keys -t smoke "go" Enter
sleep 8
tmux capture-pane -t smoke -p -e -S -    # -e keeps the SGR: the block
                                         # must carry 38;2;189;147;249
```

Each block's tag must START a line whatever the body ends with (a
search render has no trailing newline — that is the regression the
join fixes). `tools/wire-replay` and `NEVERMORE_DEBUG_WIRE=1` give the
wire half: the tool message in the recorded request carries the raw
`<system-reminder>` (never the escaped spelling) exactly once, and the
same bytes the panel shows.

## Files

| File             | Purpose                         |
| ---------------- | ------------------------------- |
| `fake_ollama.py` | the server (single stdlib file) |
| `README.md`      | this file                       |
