# nevermore

An interactive coding agent in pure C — quoth's spoken-word sibling.

_"Quoth the raven: nevermore."_

nevermore chats with AI models (Charm Hyper, Ollama local daemon and
Ollama Cloud, OpenAI, OpenRouter) from a terminal, with an agent loop
that can read, edit, and search files and run commands. It is a
first-class citizen of the [portty](../portty) terminal: kitty keyboard
protocol, OSC 52 clipboard, Lottie spinner via OSC 5555, sixel image
attach — graceful degradation everywhere else.

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

Provider keys come from the environment: `HYPER_API_KEY`,
`OLLAMA_API_KEY`, `OPENAI_API_KEY`, `OPENROUTER_API_KEY`.

## Layout

```
include/nevermore/   public headers (nm_ prefix, Nm* types)
src/                libnevermore.a: providers, wire client, transport,
                    SSE, JSON, agent loop, tools, session
cli/                the nevermore binary (modeled on ditty/cli/)
tests/              standalone test binaries (RUN_TEST/TEST_SUMMARY pattern)
docs/               wire specs ported from quoth + platform notes
data/               static model catalogs
```

See [docs/PORTTY.md](docs/PORTTY.md) for the terminal-feature matrix.

## License

MIT — see COPYING.
