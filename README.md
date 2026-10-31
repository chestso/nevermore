# nevermore

**Homepage:** <https://chest.so/nevermore/>

An interactive coding agent in pure C.

nevermore chats with AI models from a terminal. Its agent loop can read, edit
and search files, run commands, and search the web through a local SearXNG
instance. It is a first-class citizen of the
[portty](https://github.com/chestso/portty) terminal (kitty keyboard protocol,
sixel image attach) and degrades gracefully elsewhere.

- **Providers:** Charm Hyper, Ollama (`ollama:local`, `ollama:cloud`),
  OpenRouter, OpenCode (`opencode:go`, `opencode:zen`). The colon keeps the
  service and its endpoint tier distinct.
- **Transport:** hand-rolled HTTP/1.1 + SSE with the OS's own TLS. No libcurl.
- **Tools:** file read/write/edit, shell commands, long-running jobs, web
  search, skills.
- **Vision:** attach images to a prompt; image-output models render in the
  terminal.

> **Status:** pre-alpha. There is no backwards-compatibility constraint.

## Contents

- [Documentation](#documentation)
- [Install](#install)
- [Build](#build)
- [Usage](#usage)
- [Configuration](#configuration)
- [Skills](#skills)
- [Images](#images)
- [Session transcript](#session-transcript)
- [Long-running commands](#long-running-commands)
- [Smoke testing without a provider](#smoke-testing-without-a-provider)
- [License](#license)

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): what each module is, why it is shaped that
  way, and the principles behind it
- [CONTRIBUTING.md](CONTRIBUTING.md): gates, test conventions, CI, and traps
- [docs/PORTTY.md](docs/PORTTY.md): the terminal-feature matrix
- Provider API notes (live-probed wire behavior): [Hyper](docs/HYPER-API.md),
  [Ollama Cloud](docs/OLLAMA-CLOUD-API.md), [OpenCode](docs/OPENCODE-API.md),
  [OpenRouter](docs/OPENROUTER-API.md)
- [Ask DeepWiki](https://deepwiki.com/chestso/nevermore): AI-generated wiki and
  code index

## Install

**macOS (Apple Silicon) and Windows (x86_64):** download the portable ZIP from
the [releases](https://github.com/chestso/nevermore/releases) page, extract it
anywhere, and run `nevermore` / `nevermore.exe` from a terminal. There is no
installer and no MSYS2 or Homebrew. Each ZIP carries a `README-PORTABLE.txt`
that says where the two files nevermore writes live.

**GNU/Linux:** build from the source tarball on the
[releases](https://github.com/chestso/nevermore/releases) page (below).

## Build

From the source tarball, `configure` is already generated. From a git checkout,
run `./autogen.sh` first (it needs autoconf and automake).

```sh
./autogen.sh                         # git checkout only
mkdir -p build && cd build
# PKG_CONFIG_PATH: wherever boba was installed (here, under ~/.local)
PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig:$HOME/.local/lib64/pkgconfig" \
    ../configure --prefix="$HOME/.local"
make -j$(nproc)
make check
make install
```

Dependencies:

- **[boba](https://chest.so/boba)** (required): TUI runtime, text input, styles.
  A separate project, not vendored. Build and install it first; `configure`
  finds it through pkg-config.
- **TLS** (optional, per OS, never libcurl): Schannel on Windows, Secure
  Transport on macOS, mbedTLS (>= 2.28) or OpenSSL on Linux. `--with-tls=none`
  builds a plain-HTTP-only client, which is fine for a local Ollama daemon.
- **stb** (image transcoding): single-file headers fetched into
  `third_party/stb/` by the first `configure` of a git checkout (pinned commit,
  needs network, never committed); the source tarball already includes them. By
  Sean Barrett, public domain.

## Usage

```sh
nevermore                            # interactive chat
nevermore ask "explain this repo"    # one-shot
nevermore models                     # provider model catalog
nevermore -p opencode:go -m glm-5.3  # one run, ignoring the saved config
nevermore -i shot.png "what is wrong in this screenshot?"
```

`ask` and `models` are subcommands and must come **first**. A bare prompt is ask
mode: `nevermore "x"` equals `nevermore ask "x"`. A mode word anywhere else is a
usage error, not a prompt.

### API keys

Keys come from the environment (`HYPER_API_KEY`, `OLLAMA_API_KEY`,
`OPENROUTER_API_KEY`, `OPENCODE_API_KEY`) or, when the variable is unset, from
`~/.authinfo` (override with `NEVERMORE_AUTHINFO`):

```text
machine hyper.charm.land   login apikey password ...
machine ollama.com         login apikey password ...
machine openrouter.ai      login apikey password ...
machine opencode.ai        login apikey password ...
```

`ollama:local` (the default) needs no key and never reads one. Starting or
switching to a provider with no key says so up front, before the first turn.

### The chat screen

- The input row is always live. Text typed during a turn is kept for the next
  prompt; Enter does nothing until the turn ends.
- **Esc** cancels the running round (a running tool included) and nothing else.
  **Ctrl+C** also cancels; at an idle prompt it clears the text, then quits.
- **Shift+Enter** inserts a newline on terminals with the kitty keyboard
  protocol; **Ctrl+J** works everywhere.
- The status row above the prompt shows `<used>/<limit> ↓<out> ⚡<pct>` and
  `provider · model`. `used` is what the provider last reported, `limit` is the
  model's catalog window, `↓` is accumulated output tokens, `⚡` is the
  session's prompt-cache hit rate. Unknown values read `-`, never an estimate,
  and `↓` and `⚡` are absent until a round reports them. The spinner at the
  left shows what the turn is doing: a pulsing dot while the model streams,
  rotating stars while a tool runs.
- The app paints **foreground colors only**; the terminal keeps its own
  background.

#### Slash commands

- `/model`, `/provider`: switch model or provider (the model picker filters, see
  below)
- `/config`, `/config set <key> <value>`, `/config reset [key|all]`: show every
  setting and where it came from; change or reset one
- `/routing [mode]`, `/tier [name]`, `/maxprice [prompt,completion]`: the
  provider's routing mode, service tier and price ceiling (OpenRouter today; no
  argument prints the value and the accepted spellings)
- `/preset cheap|fast|balanced`: both levers at once — `cheap` is `:floor` on the
  `flex` tier, `fast` `:nitro` on `priority`, `balanced` neither
- `/context`: the exact token breakdown (last round, session totals, cache read
  rate)
- `/session`, `/session list`, `/session save [path]`: inspect or save the
  transcript as markdown
- `/image [<path>|-<n>|list|save [n] [path]]`: attach, drop, list or save images
- `/ps`, `/kill <id>`: list or stop background jobs
- `/skills`: list discovered skills and any validation findings

The `/model` picker takes free text (id substring) plus `@vision`, `@image`,
`@tools`, `tag:NAME` and `ctx:>200k`-style tokens, all ANDed. Its badges are
`ctx 👀 🖼 🔧` (window, vision, image output, tools).

### What the model sees

The system prompt is assembled once per chat and then frozen, so it stays inside
the provider's prompt cache:

- the base prompt;
- an `<env>` block: working directory, git-repo flag, platform, date, and (in a
  git repo) the branch, `git status --short` (20 lines) and the last three
  commits, gathered asynchronously while you type;
- a `<project_context>` block from `AGENTS.md` files: the global
  `~/.config/AGENTS.md`, then each `AGENTS.md` from the repo root down to the
  working directory, nearest last, capped at 32 KiB;
- a `<skills>` menu (see Skills).

The whole transcript rides every request. nevermore never trims it to a guessed
token budget, so a context that is too large is the provider's error, reported
verbatim.

The harness may add short `<system-reminder>` nudges (a truncated tool result,
the context nearing its limit, background jobs still running). **You see every
one** in the chat, in purple. Text that looks like a reminder inside tool output
or files is escaped and flagged in red, because tool output is data, never
instructions.

## Configuration

`/config` shows every setting with its current value and where it came from; the
reference below covers the same keys.

Settings resolve lowest to highest:

1. built-in defaults
2. **user config**: `~/.config/nevermore/config`, written by you
   (`$XDG_CONFIG_HOME` honored; `%USERPROFILE%\.config` on Windows)
3. **runtime shadow**: `~/.local/state/nevermore/config`, written by the chat
   (`$XDG_STATE_HOME`; `%LOCALAPPDATA%` on Windows)
4. **environment**: `NEVERMORE_*` variables named after the key, some with a
   unit suffix (`NEVERMORE_PROVIDER`, `NEVERMORE_MODEL`, `NEVERMORE_TIMEOUT_MS`,
   `NEVERMORE_SEARXNG_URL`, ...)
5. **command line**: `-p`, `-m`

The environment outranks both files on purpose, so a scripted
`NEVERMORE_MODEL=x nevermore` is never overridden by what you last typed in a
chat; the chat tells you when a change is inert for that reason. Changes made in
the chat (`/config set`, `/model`, `/provider`) go to the shadow file only and
never touch your config. `rm ~/.local/state/nevermore/config` (or
`/config reset all`) puts your config back in charge.

The file format is `key = value`, one per line. `#` starts a comment line, the
value is the rest of the line taken verbatim (no quoting, no inline comments),
and unknown keys or bad values warn and are skipped. No secrets belong here.

```ini
# ~/.config/nevermore/config
provider          = opencode:go
model.opencode:go = glm-5.3
rounds            = 40
searxng           = http://127.0.0.1:8888
```

A model is remembered **per provider** — `model.<provider>` is the only
spelling; there is no plain `model` key. With no model set for the active
provider, nevermore asks you to choose one with `/model` instead of guessing an
id from another provider's catalog. `-m ID` / `$NEVERMORE_MODEL` set the model
for this run only, for the provider the run resolved.

- `provider` (default `ollama:local`): active provider
- `model.<provider>` (default none): model id, remembered per provider
- `rounds` (default 25): max tool rounds in one turn; past it the turn stops
  with "too many tool rounds"
- `void_retries` (default 2, range 0-9): how often a void round (one that came
  back with no text and no tool call) is re-asked; every void round is shown in
  red, `0` ends the turn at the first one
- `temperature` (default unset, range 0-2): unset means the field is not sent
  and the provider decides
- `max_tokens` (default unset): output cap per round; unset means not sent
- `timeout` (default 300000 ms): stream inactivity budget; any wire byte
  (including SSE keep-alives) resets it; `off` disables
- `run_command_timeout` (default 300000 ms): the same for `run_command`: a child
  silent that long is stopped and its partial output returned; `off` disables
- `connect_timeout` (default 750 ms): per-address budget when dialing a host's
  addresses in turn
- `handshake_timeout` (default 10000 ms): TLS handshake budget; `off` restores
  the OS default
- `family_skip` (default off): once one IP family (IPv4 or IPv6) burns the
  connect budget and the other answers, dial the slow family last for the
  session
- `skip_families` (default `none`): the latch `family_skip` writes at runtime to
  remember which family to dial last; inert while `family_skip` is off
- `searxng` (default `http://127.0.0.1:8888`): SearXNG endpoint for `web_search`
- `searxng_enabled` (default on): set to off at runtime when the instance is
  unreachable; `/config reset searxng_enabled` re-arms it
- `searxng_timeout` (default 10000 ms): per-query budget
- `reasoning_echo`, `signature_echo` (default `all`): what reasoning data is
  sent back to the provider: `off`, `tools` or `all` (see below)
- `login_shell` (default off): let `exec_command` run a login shell that sources
  your profile; a call's own `login: false` always wins
- `reminders` (default on): the harness's nudges; the escaping of forged tags is
  never switchable
- `kbd` (default `auto`): kitty keyboard protocol: `auto` (after the terminal
  proves it), `on`, `off`
- `color` (default `auto`): `auto`, `16` (ANSI palette) or `rgb` (24-bit);
  `auto` goes by `COLORTERM`, and `rgb` is the escape hatch
- `spinner_ramp` (default on): the thinking dot breathes `· • ● •`; off holds
  one `●`
- `prism_notice` (default on): Hyper's `prism` models are a server-side router
  that picks an upstream model per request; print which one answered, once per
  routed round
- `served_notice` (default on): print which upstream served a round and at which
  service tier (`openrouter: served by Azure (flex tier)`), when it carries
  signal — a non-`default` tier, or an upstream that changed. `/context` reports
  the same facts with the key off
- `skills` (default on): skill discovery, read when a chat starts
- `image_live` (default 1): how many requests an image rides before it becomes a
  text marker; `off` keeps it on every request

Environment-only knobs: `NEVERMORE_CONFIG` and `NEVERMORE_SHADOW_CONFIG`
relocate the two files, `NEVERMORE_BASE_URL` overrides the endpoint for one run,
`NEVERMORE_APP_URL` changes the app URL OpenRouter attributes traffic to (an
absolute `http(s)` URL, else ignored), `OPENROUTER_PROMPT_CACHE=off|5m|1h` picks
the prompt-cache TTL OpenRouter's Anthropic models are asked for (`off` sends
no marker), and `NEVERMORE_DEBUG_WIRE=1` records every request body and stream
event verbatim under `$XDG_STATE_HOME/nevermore/wire/`.

### Routing, tier and price

OpenRouter hosts one model on many upstreams and routes each request to one of
them. nevermore can steer that, with three keys that belong to a provider (so
each is spelled `<key>.<provider>`, and a provider that has no such vocabulary
refuses them):

- `routing.<provider>`: `balanced` (the provider's own routing), `nitro` (sort
  by throughput, admitting priority-tier endpoints), `floor` (sort by price,
  admitting flex-tier endpoints) or `exacto` (quality signals tuned for tool
  use). The mode rides the model ID as its last suffix, never the body: the
  suffix is the provider's own semantics, and the catalog lookup drops it again,
  so a routed model keeps its window and badges.
- `tier.<provider>`: the service tier the round is billed at: `default`, `flex`,
  `priority` (the wire's `fast` means the same and is stored as `priority`), or
  `ultrafast`. Unset is NOT `default`: it means whatever the provider's
  workspace says, else the router's own. `ultrafast` is refused unless
  `max_tokens` is set, because without an output cap the provider silently falls
  back to `priority` — a knob that mostly lies is worse than none.
- `max_price.<provider>`: a ceiling as `<prompt>,<completion>` in $/Mtok, e.g.
  `3,15` means at most $3 per million input tokens and $15 per million output
  tokens. Unlike a preference it can PREVENT the request: if no endpoint fits,
  the round fails with the provider's own message.

```ini
routing.openrouter = nitro
tier.openrouter    = flex
max_price.openrouter = 3,15
```

In the TUI the same three are `/routing`, `/tier` and `/maxprice` (no argument
prints the value and the accepted spellings), and `/preset cheap|fast|balanced`
writes the two common pairs at once. There is no environment spelling for these
keys: a scoped key is the provider's, and a scripted run writes the user config.
A change applies to the very next round — none of the three is part of the
prompt, so the provider's cache is untouched and the conversation id is not
rotated.

### Reasoning echo

Reasoning models send two things besides the answer: a trace
(`reasoning_content`) and an opaque structured carrier (`reasoning_details`,
e.g. a Gemini thought signature). Both are always received and the trace is
shown; the two keys decide what rides back on later requests:

- `off`: nothing goes back.
- `tools`: only assistant messages that carry tool calls.
- `all` (default): every assistant message that has one.

Some wires need the echo: `opencode:go`'s deepseek endpoint returns an
intermittent 400 on a tool round missing `reasoning_content`, and OpenRouter's
Gemini models require their thought signature back with the tool result. If
nothing is failing and you pay per token on a long session, `off` is cheaper.
Each knob freezes for the rest of a chat once a request has carried its field
(changing it mid-chat would invalidate the prompt cache), and `/config` shows
what the next request will actually send.

### Prompt cache and usage

Hyper, OpenRouter and the OpenCode tiers report cache reads; the OpenCode
upstreams also report cache writes; Ollama reports token counts only. Where a
fact is not reported the display degrades instead of inventing it.

## Skills

A skill is a folder holding a `SKILL.md` (YAML frontmatter with `name` and
`description`, instructions in the body) in the open
[Agent Skills](https://agentskills.io) format.

```text
~/.agents/skills/<name>/SKILL.md           yours, every project
<dir>/.agents/skills/<name>/SKILL.md       the project's, from the repo root down to the cwd
```

A project skill overrides a user skill of the same name, and the nearest
directory wins within a project. `.claude/skills/` is not read; link it into
`~/.agents/skills/` if you want it.

Only each skill's name and description go into the prompt. When a task matches,
the model calls `load_skill` and gets the body as an ordinary tool result.
nevermore runs nothing on a skill's behalf: a script a skill mentions goes
through `run_command`. `/skills` lists what was found, where, and every
validation finding or refusal. With no skills, the menu and the `load_skill`
tool are absent.

## Images

**Attach:** `/image <path>` attaches an image to the next message (repeatable);
`-i <path>` does the same in ask mode. PNG, JPEG and GIF up to 8 MiB are
accepted. The file is read once at attach and frozen into the session, so later
rounds never re-read it. On a graphics terminal (kitty, iTerm2/WezTerm, sixel)
the image shows right under the attach line; elsewhere you get a one-line
marker.

**Age-out:** an image rides the request that introduced it and the next
`image_live - 1` requests. After that it becomes a short frozen text marker such
as `[image shot.png — PNG 800x600, 42.0 KiB — attached earlier, not repeated]`.
The swap costs one re-bill of the prompt tail and keeps the cache valid after
it.

**Vision flag:** sending an image to a text-only model is only a warning, never
a refusal.

**Generated images:** image-output models (marked 🖼 in `/model`) return a
picture that renders like an attachment and is captioned `image #N`. Save one
with `/image save [n] [path]` (default `nevermore-image-<n>.<ext>` in the cwd).
Ask mode saves it automatically and keeps stdout clean. The model cannot save
images itself.

## Session transcript

```text
/session              # shape: messages by role, size, images
/session list         # every message, numbered
/session save [path]  # markdown, default nevermore-session.md
```

Message 0 is the assembled system prompt. The saved file is markdown (prose as
written, system prompt and tool results in fenced blocks) with a `<!-- meta -->`
line per message carrying exact body byte counts. Images are not written; use
`/image save`. Loading a saved transcript back is not implemented yet.

## Long-running commands

- `run_command`: one short, non-interactive command, one result. Its output
  opens with `Process exited with code N`.
- `exec_command`: anything long-lived or interactive (dev server, REPL, `ssh`,
  test watcher). It returns the exit status, or a job id once its yield window
  (capped at 30 s) closes. `shell` and `login` arguments choose the interpreter
  and whether to source your profile (`login_shell` gates the latter).
- The model drives a job with `write_stdin` (feed input, read new output; an
  empty poll waits up to 5 minutes) and `kill_job`. Job output is buffered for
  the model and never streamed into the transcript.
- You watch the same jobs with `/ps` and stop one with `/kill <id>` (the shell
  and its descendants).
- At most 31 jobs run at once, and they die with nevermore.

## Smoke testing without a provider

The test suite never uses the network. For a manual pass, use the fake provider
and wire replay in `tools/`; the recipes are in
[CONTRIBUTING.md](CONTRIBUTING.md#manual-smoke-without-a-provider).

## License

MIT, see COPYING.
