# fake-ollama — a daemon stand-in for the TUI smoke

A dependency-free Python HTTP server that speaks just enough of the
local Ollama surface for nevermore's manual smoke:

- `GET /api/tags` — a one-model catalog
- `POST /v1/chat/completions` — a paced SSE stream

It exists because a real local daemon is not available on every dev
box, and a manual TUI smoke should not depend on one. `-p ollama-local`
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
| `--scenario`  | `table-fence` (default) or `prose`                           |

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

Deltas are split so markdown arrives incrementally (lines/rows land
across separate SSE events), as a real stream would.

## The smoke recipe (tmux, not a raw pty)

```sh
tools/fake-ollama/fake_ollama.py --both --pace 0.6 &
tmux new-session -d -s smoke -x 90 -y 28
tmux send-keys -t smoke \
    "NEVERMORE_AUTHINFO=/dev/null ./build/src/nevermore -p ollama-local" Enter
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

## Files

| File             | Purpose                         |
| ---------------- | ------------------------------- |
| `fake_ollama.py` | the server (single stdlib file) |
| `README.md`      | this file                       |
