# nevermore

An interactive coding agent in pure C — quoth's spoken-word sibling.

_"Quoth the raven: nevermore."_

nevermore chats with AI models (Charm Hyper, Ollama local daemon and
Ollama Cloud, OpenAI, OpenRouter, OpenCode Go and Zen) from a
terminal, with an agent loop that can read, edit, and search files,
run commands, and search the web through a local SearXNG instance. It
is a first-class citizen of the [portty](../portty)
terminal: kitty keyboard protocol, OSC 52 clipboard, Lottie spinner
via OSC 5555, sixel image attach — graceful degradation elsewhere.

Provider names carry the endpoint tier explicitly: `ollama:cloud`,
`ollama:local`, `opencode:go`, `opencode:zen`. A bare noun or a dash
suffix reads as a separate service; the colon keeps the service and
the tier distinct.

Pre-alpha. No backwards-compatibility constraint.

## Build

Autotools, house style:

```sh
./autogen.sh
mkdir -p build && cd build
PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig:$HOME/.local/lib64/pkgconfig" \
    ../configure --prefix="$HOME/.local"
make -j$(nproc)
make check
make install
```

Or from the chestso monorepo root: `make nevermore`.

Dependencies:

- **boba** (required) — TUI runtime, textinput, styles
- **TLS** (optional, per-OS, never libcurl) — Schannel on Windows,
  Secure Transport on macOS, mbedTLS or OpenSSL on Linux; `--with-tls=none`
  builds a plain-HTTP-only client (fine against a local Ollama daemon)

## Usage

```sh
nevermore                          # interactive chat (boba TUI)
nevermore ask "explain this repo"  # one-shot
nevermore models                   # provider model catalog
NEVERMORE_PROVIDER=openrouter nevermore ask "..."
nevermore -p openai -m glm-5.3     # one run, ignoring the saved config
```

Provider keys come from the environment (`HYPER_API_KEY`,
`OLLAMA_API_KEY`, `OPENAI_API_KEY`, `OPENROUTER_API_KEY`,
`OPENCODE_API_KEY`) or, when an
env var is unset, from `~/.authinfo` — one `machine <name> password
<secret>` line per provider:

```
machine hyper.charm.land   login apikey password ...
machine ollama.com         login apikey password ...
machine openai.com         login apikey password ...
machine openrouter.ai      login apikey password ...
machine opencode.ai        login apikey password ...
```

Environment variables win over the file. Point elsewhere with
`NEVERMORE_AUTHINFO=/path/to/authinfo`. `ollama:local` (the zero-config
default) needs no key: use `-p ollama:cloud` for Ollama Cloud.

## Configuration

Settings resolve once, lowest to highest:

1. built-in defaults
2. **user config** — `~/.config/nevermore/config`, yours, written by
   hand (`$XDG_CONFIG_HOME` honored; `%USERPROFILE%\.config` on Windows)
3. **runtime shadow** — `~/.local/state/nevermore/config`, written by
   the chat (`$XDG_STATE_HOME` honored; `%LOCALAPPDATA%` on Windows)
4. **environment** — `NEVERMORE_PROVIDER`, `NEVERMORE_MODEL`,
   `NEVERMORE_MAX_ROUNDS`, `NEVERMORE_ECHO_REASONING`,
   `NEVERMORE_CONNECT_TIMEOUT_MS`, `NEVERMORE_CONNECT_FAMILY_SKIP`,
   `NEVERMORE_SEARXNG_URL`
5. **command line** — `-p` / `-m`

The environment deliberately outranks both files: a scripted
`NEVERMORE_MODEL=x nevermore` must not be silently overridden by what
you last typed in a chat. When a change is inert for that reason, the
chat says so.

A change made in the chat (`/model`, `/provider`, `/rounds`,
`/reasoning`, `/connect`) never edits your config file. It is written
to the shadow file, which holds only the keys you changed at the
prompt — so `rm ~/.local/state/nevermore/config` (or `/config reset
all`) puts the user config back in charge, with nothing else to unwind.

```
# ~/.config/nevermore/config — '#' comment, blank lines ignored
provider         = openai
model            = glm-5.3
rounds           = 40
reasoning        = on
connect_timeout  = 1500
family_skip      = on
searxng          = http://127.0.0.1:8888
```

Seven keys, one spelling each. The value is the rest of the line,
trimmed and taken verbatim — no quoting, no inline comments. Unknown
keys and invalid values warn and are skipped, so a stale file can never
break startup. No secrets: API keys stay in the environment or
`~/.authinfo`.

`searxng` is the local [SearXNG](https://searxng.org) endpoint behind
the `web_search` tool (default `http://127.0.0.1:8888`); the model
queries it when it needs live web results. If the instance is
unreachable, `web_search` says so once and short-circuits for the rest
of the session rather than hammering a dead server.

`connect_timeout` is the per-address budget in milliseconds for the
bounded connect walk (default 750): a hostname resolves to several
addresses and each is dialled in turn, so a black-holed one — the
classic unroutable IPv6 on a v4-only network, no RST and no SYN-ACK —
is abandoned after the budget instead of the OS's ~130 s. `family_skip`
(a bool) goes one step further: once an address of a family burns the
budget and an address of _another_ family then answers, that family is
not dialled again for the rest of the session, so later connects pay no
budget at all. It only fires on that evidence — a walk that failed
everywhere latches nothing. `/connect` reports and sets both
(`/connect 1500`, `/connect on`, `/connect reset`).

`NEVERMORE_CONFIG` / `NEVERMORE_SHADOW_CONFIG` point the two files
elsewhere (e2e and replay rigs). `NEVERMORE_BASE_URL` overrides the
endpoint for one run — a testing knob, deliberately not a config key.

In the chat: `/config` shows where each setting comes from,
`/config reset [key|all]` clears shadow lines, and `/rounds reset` /
`/reasoning reset` / `/connect reset` do the same for one key.

## Long-running commands

`exec_command` starts a command that outlives the tool call: a dev
server, a REPL, `ssh`, a test watcher. It reports either the exit
status (the command finished inside its yield window) or a job id.

`run_command` is the sibling for the other case: one short,
non-interactive command, one result — no job, no terminal, no stdin.
A one-shot `ls`, `grep` or `make` is a `run_command`; anything that
might still be running, or that expects a terminal, is an
`exec_command`.

The job keeps running between turns, and the model drives it on its
own: `write_stdin` feeds it input and reports what it has printed since,
`kill_job` stops it. Output produced between calls is buffered for
the model to poll. Its output is deliberately **not** streamed
into the transcript — it is the model's to poll, so a build log does
not scroll by unasked.

You watch the same jobs from the chat:

```
/ps            # id, state (running / exited N), command, output buffered
/kill 3        # stop one: the shell AND its descendants (group-kill)
```

The spinner keeps ticking while a command runs, so a silent child never
looks like a hang.

Jobs run on both platforms, over the shell each one's spawn uses
(`/bin/sh -c` on POSIX; `cmd.exe /d /c` on Windows, whose jobs are
pipes plus a Job Object for the group kill). They are process-global
and each occupies a slot in the event loop's I/O-source pool, so at
most `NM_PROC_MAX_JOBS` (31: the pool less the agent's own source)
run at once — one past that fails loudly rather than starting a child
nothing would read. They die with nevermore.

## Layout

```
src/                the nevermore binary: entry point, config, providers,
                    wire client, transport, SSE, JSON, agent loop, tools,
                    session, TUI (headers live alongside sources —
                    CLI app, no library)
tests/              standalone test binaries (RUN_TEST/TEST_SUMMARY pattern)
tools/wire-replay/  replay server for captured wire dumps (Python)
docs/               wire specs ported from quoth + platform notes
data/               static model catalogs
```

See [docs/PORTTY.md](docs/PORTTY.md) for the terminal-feature matrix.

## License

MIT — see COPYING.
