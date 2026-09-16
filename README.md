# nevermore

An interactive coding agent in pure C — quoth's spoken-word sibling.

_"Quoth the raven: nevermore."_

nevermore chats with AI models (Charm Hyper, Ollama local daemon and
Ollama Cloud, OpenAI, OpenRouter, OpenCode Go and Zen) from a
terminal, with an agent loop that can read, edit, and search files and
run commands. It is a first-class citizen of the [portty](../portty)
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

`NEVERMORE_PROVIDER` / `NEVERMORE_MODEL` pick the default provider and
model. `NEVERMORE_MAX_ROUNDS=<n>` caps the tool-call rounds in one turn
(default 25) before the agent stops with "too many tool rounds"; the
interactive chat also exposes it as `/rounds [n|default]`.

## Layout

```
src/                the nevermore binary: entry point, providers, wire
                    client, transport, SSE, JSON, agent loop, tools,
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
