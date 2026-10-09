# nevermore

An interactive coding agent in pure C.

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

Autotools:

```sh
./autogen.sh
mkdir -p build && cd build
PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig:$HOME/.local/lib64/pkgconfig" \
    ../configure --prefix="$HOME/.local"
make -j$(nproc)
make check
make install
```

Dependencies:

- **boba** (required) — TUI runtime, textinput, styles
- **TLS** (optional, per-OS, never libcurl) — Schannel on Windows,
  Secure Transport on macOS, mbedTLS (≥ 2.28) or OpenSSL on Linux;
  `--with-tls=none` builds a plain-HTTP-only client (fine against a
  local Ollama daemon)

## Usage

```sh
nevermore                          # interactive chat (boba TUI)
nevermore ask "explain this repo"  # one-shot
nevermore models                   # provider model catalog
NEVERMORE_PROVIDER=openrouter nevermore ask "..."
nevermore -p openai -m glm-5.3     # one run, ignoring the saved config
nevermore -i shot.png "what is wrong in this screenshot?"
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

Starting (or switching to) a provider that needs a key with none
configured says so up front — `export OPENAI_API_KEY`, or add the
`~/.authinfo` line — instead of failing on the first turn. The check
runs through the same key lookup as the request, so a `~/.authinfo`
entry counts.

The input row is always on screen, busy or not: it gathers the next
prompt while a turn runs (keys edit it; Enter is a no-op until the turn
ends; Ctrl+C interrupts). Above the prompt its status row carries the
context gauge, `ctx <used>/<limit>`, where the used count is the prompt
the provider last reported and the limit the active model's catalog
window — plus, once some round has reported a cache read, the session's
cache rate `⚡<pct>`: the share of this conversation's input that came
from the provider's prefix cache, accumulated over every completed round
that reported a read count (a round that omits the field is left out of
the rate, never counted as a miss). The write side is tracked separately
and not rated. An unknown number reads as `-`, never an estimate, and
the `⚡` marker is simply absent until a read count exists. The row's
right end names the endpoint it is talking to — `provider · model` —
elided from the tail when the terminal is narrow, and dropped when there
is no room for it at all; what the turn is DOING is the spinner glyph at
the row's left (the braille tier animates while the model streams, the
charset tier while a tool runs), so there is no word beside it to go
stale between states. `/context` prints the exact breakdown — the last
round's numbers plus the session totals, the cache read (and its rate),
and the cache write count.

Providers differ in what they report: OpenAI, Hyper, OpenRouter and the
OpenCode tiers report a cache read (`prompt_tokens_details.cached_tokens`)
— Hyper's is occasionally absent for a round — the OpenCode upstreams
also report a cache write, and Ollama reports the token counts but no
cache breakdown. Every provider's usage object is folded into one
canonical set by the shared client; where a fact is not reported the
display degrades (no `⚡`, `session cache: not reported`), it is never
invented.

Every request's system message is assembled once per chat: the base
prompt, an `<env>` block (working directory, whether it is a git repo,
platform, today's date), and a `<project_context>` block built from the
`AGENTS.md` files that apply to the working directory — the global
`~/.config/AGENTS.md` first, then each `AGENTS.md` from the project root
(the nearest ancestor with a `.git`) down to the cwd, so the nearest file
comes last and wins by recency. Files are labeled with their path and
the whole block is capped at 32 KiB with an in-band truncation notice.
Inside a git repo the `<env>` block also carries a git snapshot — the
current branch, `git status --short` (first 20 lines), and the last three
commits. That snapshot needs a subprocess, so it runs **asynchronously**
while you type: the first round waits for it (bounded at 10 s) before the
prompt goes out, and a hung or failed `git` simply leaves the section out.
The assembled prompt is then frozen for the chat's life, which is what
keeps it inside the provider's cached prefix.

## Configuration

Settings resolve once, lowest to highest:

1. built-in defaults
2. **user config** — `~/.config/nevermore/config`, yours, written by
   hand (`$XDG_CONFIG_HOME` honored; `%USERPROFILE%\.config` on Windows)
3. **runtime shadow** — `~/.local/state/nevermore/config`, written by
   the chat (`$XDG_STATE_HOME` honored; `%LOCALAPPDATA%` on Windows)
4. **environment** — `NEVERMORE_PROVIDER`, `NEVERMORE_MODEL`,
   `NEVERMORE_MAX_ROUNDS`, `NEVERMORE_REASONING_ECHO`,
   `NEVERMORE_TIMEOUT_MS`, `NEVERMORE_CONNECT_TIMEOUT_MS`,
   `NEVERMORE_CONNECT_FAMILY_SKIP`, `NEVERMORE_CONNECT_SKIP_FAMILIES`,
   `NEVERMORE_SEARXNG_URL`, `NEVERMORE_SEARXNG_ENABLED`,
   `NEVERMORE_SEARXNG_TIMEOUT_MS`, `NEVERMORE_RUN_COMMAND_TIMEOUT_MS`
5. **command line** — `-p` / `-m`

The environment deliberately outranks both files: a scripted
`NEVERMORE_MODEL=x nevermore` must not be silently overridden by what
you last typed in a chat. When a change is inert for that reason, the
chat says so.

A change made in the chat (`/config set`, `/model`, `/provider`)
never edits your config file. It is written to the shadow file, which
holds only the keys you changed at the prompt — so `rm
~/.local/state/nevermore/config` (or `/config reset all`) puts the user
config back in charge, with nothing else to unwind.

```
# ~/.config/nevermore/config — '#' comment, blank lines ignored
provider         = openai
model            = glm-5.3
rounds           = 40
reasoning_echo   = tools
timeout          = 300000
connect_timeout  = 1500
handshake_timeout = 10000
family_skip      = on
skip_families    = none
searxng          = http://127.0.0.1:8888
searxng_enabled  = on
searxng_timeout  = 10000
run_command_timeout = 300000
login_shell      = off
reminders        = on
```

The value is the rest of the line, trimmed and taken verbatim — no
quoting, no inline comments. Unknown keys and invalid values warn and
are skipped, so a stale file can never break startup. No secrets: API
keys stay in the environment or `~/.authinfo`.

`model` is remembered **per provider**: a model id belongs to one
provider, so a scoped `model.<provider>` line (e.g. `model.openai =
glm-5.3`) is what the chat writes when you pick a model, and it
outranks the plain `model`. `-m` and `$NEVERMORE_MODEL` stay global
(an explicit flag is not memory). With no model set for the active
provider, nevermore says so and waits for `/model` — ask mode exits
before any traffic — rather than guessing an id from another
provider's catalog.

The whole transcript rides every request: nevermore never trims it to a
guessed token budget (a sliding window would silently cap the
conversation and change the request prefix the provider's prompt cache
keys on). A context that is too large is therefore the provider's error
to report, verbatim; `/context` shows the provider-reported usage that
tells you how close you are.

`searxng` is the local [SearXNG](https://searxng.org) endpoint behind
the `web_search` tool (default `http://127.0.0.1:8888`); the model
queries it when it needs live web results. `searxng_enabled` (a bool)
is the tool's gate: when the instance is unreachable, `web_search`
says so once and the tool writes `searxng_enabled = off` on the
**runtime** layer, so later calls short-circuit instead of hammering a
dead server — `/config` shows the value and its `(runtime)` layer, and
`/config reset searxng_enabled` (or fixing the endpoint) re-arms it.
`searxng_timeout` is the per-request budget in milliseconds (default
10000): a query whose connection is accepted but never answered is
abandoned after it instead of hanging the turn.

`timeout` is the stream-inactivity budget in milliseconds (default
300000): while a round is streaming, no wire byte for that long fails
the turn with `timed out` instead of hanging. It counts _bytes_, not
events — every delta resets it, and so do the SSE keep-alive comments
that bridge a minutes-long image-generation gap — so a long-but-live
answer is never cut. `run_command_timeout` (default 300000) is the
same shape for the `run_command` tool: a child that produces no output
for that long is stopped and its partial output returned. Either key
takes `off` to disable the deadline entirely (a purely readiness-driven
wait).

`connect_timeout` is the per-address budget in milliseconds for the
bounded connect walk (default 750): a hostname resolves to several
addresses and each is dialled in turn, so a black-holed one — the
classic unroutable IPv6 on a v4-only network, no RST and no SYN-ACK —
is abandoned after the budget instead of the OS's ~130 s.
`handshake_timeout` (default 10000, or `off`) is the connect phase's
second half: the TLS handshake runs on a non-blocking socket and each
backend waits for readiness against this budget, so a peer that
completes the TCP handshake and then goes silent — a wedged middlebox,
a route black-holed mid-exchange — costs the budget instead of sitting
in a blocking read until the OS gives up (minutes). `off` restores
that OS default. `family_skip`
(a bool) goes one step further: once an address of a family burns the
budget and an address of _another_ family then answers, that family is
dialled _last_ for the rest of the session, so later connects pay no
budget at all whenever the other family answers. The latch is a
preference, never a veto: the deferred family's addresses stay in the
walk, so a name whose only address is of that family (a `127.0.0.1`
literal, an IPv6-only service) stays reachable — wrong evidence costs a
budget, never a host. It only fires on that evidence (a walk that
failed everywhere latches nothing), and it is the one switch: with
`family_skip` off a latched value has no effect at all, so a
`skip_families` line left in a config file cannot outvote it. The
latched set is the `skip_families` key (`none`, `ipv4`, `ipv6`,
`ipv4+ipv6`), which the walk writes on the runtime layer and `/config`
shows like any other value — marked `inert: family_skip off` when the
policy is off.

`reasoning_echo` (`$NEVERMORE_REASONING_ECHO`) decides whether a round's
thinking trace is re-sent to the
provider as `reasoning_content` on the assistant messages of later
requests. The trace is always received, shown (dimmed) and kept in the
session; only the wire changes. The **provider decides the default** —
a provider whose wire 400s a replayed tool-call turn that omits the
field declares it, and `opencode:go` declares `tools` (its `deepseek`
endpoint is the one probed to demand it, so a `deepseek` model on
`opencode:go` stops returning an intermittent 400 on tool rounds) — and
this key is the override, winning whenever it is set at any layer.
`off` sends none, `tools`
sends the ones on messages that carry `tool_calls` —
and `all` sends every trace. (`on`/`true`/`1` are accepted for `all`.)
Once a request has actually carried a trace the mode is **frozen for
that chat**: a prefix that gains or loses the field is a different
prefix, so changing the key mid-conversation would throw the provider's
prompt cache away; the change applies to the next chat, and `/config`
says so.

`NEVERMORE_CONFIG` / `NEVERMORE_SHADOW_CONFIG` point the two files
elsewhere (e2e and replay rigs). `NEVERMORE_BASE_URL` overrides the
endpoint for one run — a testing knob, deliberately not a config key.

`login_shell` (a bool, default `off`) is whether `exec_command` may run
a login shell: one that sources your profile, so the PATH and aliases
in it are yours. It is off because a profile is code you wrote for
yourself, not something a model call should run by default; turn it on
when jobs cannot find your toolchain (a TUI started from a launcher has
a minimal PATH). A call's own `login: false` always wins, and the key
never reaches `run_command` or nevermore's own stages — those run under
the platform shell, deterministically, whatever this is set to.

`reminders` (a bool, default `on`) is the gate for the harness's own
nudges: short `<system-reminder>` blocks injected into the conversation
when a condition is met — a tool result that was truncated, a partial
`read_file` window, a file that turned out to be empty or an offset
past its last line, an image attached for a model that cannot see it,
web results that came from outside the machine, the context gauge
crossing 85 %/95 %, background jobs still running from earlier turns,
an answer the output limit cut short, the last tool round of a turn.
They are **never silent**: the human sees every one (the user-channel
ones as a purple `reminder (rule): …` line, the tool-channel ones
inside the panel's own body, in the same purple role), because the
transcript must never diverge from what the model received. Turn it
`off` to silence the nudges. The **trust boundary is not gated**: any
`<system-reminder>`-looking text arriving _inside_ tool output, file
contents or search results is escaped as `&lt;system-reminder>` (so it
can never be mistaken for harness speech) and the user is warned in red
that something tried — the model is told, by the system prompt, that
such text is data. That is also why a file that documents the tag reads
escaped.

In the chat: `/config` shows every key's effective value and where it
comes from (including machinery-written runtime values), `/config set
<key> <value>` writes one key to the shadow, and `/config reset
[key|all]` drops both the shadow line and any runtime value for the
key.

## Images (vision models)

A turn may carry images. In the chat, `/img <path>` attaches one to
the NEXT message (repeat for several), `/img` lists what is pending,
and `/img -<n>` drops one; in ask mode, `-i <path>` (repeatable) does
the same for the one-shot prompt:

```
/img ~/shot.png
what is wrong in this screenshot?     # the pending image rides this turn
```

The file is read **once, at attach**: the bytes are frozen into the
session as a canonical `data:` URL, so later rounds never re-read it.
That is not an optimization — the file can change under you, and the
provider's prompt cache keys on the serialized request bytes, so a
re-read would silently swap the image _and_ throw the cached prefix
away. The transcript shows the captured bytes (the same IMAGE block the
model's own images render as), never the path.

A terminal that speaks a graphics protocol (kitty, iTerm2/WezTerm) shows
the image **right at the attach**, under the line that names it — you
see what you picked. The image is shown exactly once: the turn that
carries it does not re-echo it. On a terminal without graphics support
(or when the probe has not answered yet) the attach stays a text line
and the message's echo renders the image's marker — alt, format, dims,
size and the reason — never the payload.

`/img` refuses a file that is unreadable, is not a PNG/JPEG/GIF, or is
over 8 MiB — the bytes ride **every** request, so the cap bounds the
body, and a local refusal keeps the message yours instead of a provider
error. That is the only cap: an image the attach accepted renders,
however large, because the transcript does not second-guess a payload
that is already in the conversation.

The model's own `read_file` meets the same three containers, and a file
it can see is an image but the wire cannot take (a WebP, say) is named
as such — container, dimensions, size, and what would work — instead of
being reported as unreadable text. Attachable containers are PNG, JPEG
and GIF; anything else has to be converted first. A file that is not
text at all is named too (a PDF, a ZIP, an ELF binary, or plainly
"binary file") rather than reported as a UTF-8 problem.

Sending an image to a text-only model is not an error — the provider
strips it and the model answers blind — so the catalog's vision flag is
a **warning**, never a refusal: `/img` notes it, and `/model` notes it
when the conversation already carries images.

## Generated images (image models)

The `/model` picker marks image-output models with 🖼 (the catalog's
`image_gen` flag — openrouter's `architecture.output_modalities`). Pick
one and just ask: the model's image arrives as one stream event, is
stored **verbatim** (the received bytes are the canonical part — a
re-encode would change the replay prefix), and renders through the same
pipeline as an attachment: the picture on a graphics terminal, the
one-line marker (alt · format · dims · size) elsewhere. Each received
image prints a caption with its number (`image #3`) — a picture carries
no text of its own, so that line is how it is named again. In ask mode
there is no transcript, so the image is saved to a file —
`nevermore-image-N.png` in the cwd, noted on stderr — and stdout stays
clean.

Keeping one is `/save`:

```
/save            # the newest image -> nevermore-image-<n>.<ext> here
/save 3          # the image the caption called #3
/save 3 ~/a.png  # ...where you say
/save list       # every image in the conversation, with its number
```

The numbers are the chat's image order (attachments and generated
images share it), and `/save` writes the conversation's own bytes —
exactly what the wire replays, never a re-encode. This exists because
the model cannot do it: an image-output model's catalog row usually
lists no tool support, so the request carries no toolset at all, and
"save this image" can only be answered with another picture. The
command is the honest path; the caption is what makes it addressable.

Editing is the next turn ("make it blue"): the assistant message
replays with its images at message level — the providers' own shape,
not content-parts — and the prefix-cache byte-stability invariant the
sending side keeps applies unchanged. A model answering with a remote
URL instead of inline bytes gets a notice, not a fetch: nevermore
downloads nothing. Image output tokens are completion tokens (never
cacheable — the ⚡ rate is input-only by definition), and a received
image is never refused for size: the wire cap bounds what _you_ send,
not what the model made.

## Long-running commands

`exec_command` starts a command that outlives the tool call: a dev
server, a REPL, `ssh`, a test watcher. It reports either the exit
status (the command finished inside its yield window) or a job id — and
when the requested window was clamped to the 30 s cap, the report says
so (`yield window clamped from 120000 to 30000 ms`), so a model that
asked for minutes is never left guessing why it was bounced early.

`run_command` is the sibling for the other case: one short,
non-interactive command, one result — no job, no terminal, no stdin.
A one-shot `ls`, `grep` or `make` is a `run_command`; anything that
might still be running, or that expects a terminal, is an
`exec_command`.

`exec_command` takes a `shell` and a `login` argument. `shell` names
the interpreter (a path or a bare name — `bash`, `/usr/bin/zsh`,
`pwsh`); the default stays the platform's deterministic shell
(`/bin/sh`, `cmd.exe`), which is what nevermore's own machinery
assumes, so naming one is opt-in. `login` runs the shell with login
semantics, sourcing your profile so the PATH, aliases and version
managers it sets actually apply — the fix for a TUI launched from a
desktop launcher or a session manager, where `cargo`, `nvm` or `brew`
are simply not on the job's PATH. It is off unless you turn it on
(`login_shell = on`); a call's own `login: true` still needs the gate,
and on a shell with no login mode (`cmd.exe`) a requested login is
refused by name rather than silently ignored. Profile chatter lands in
the job's output — the model reads it once. Neither argument affects
`run_command`, which is always one short command under the platform
shell.

The job keeps running between turns, and the model drives it on its
own: `write_stdin` feeds it input and reports what it has printed since,
`kill_job` stops it. The `write_stdin` window is per mode: a non-empty
write caps at 30 s, while an empty poll waits 5 s up to the
`poll_timeout` ceiling (5 min by default) — the patient way to wait out
a build. Output produced between calls is buffered for
the model to poll. Its output is deliberately **not** streamed
into the transcript — it is the model's to poll, so a build log does
not scroll by unasked.

You watch the same jobs from the chat:

```
/ps            # id, state (running / exited N), command, output buffered
/kill 3        # stop one: the shell AND its descendants (group-kill)
```

The spinner keeps ticking while a command runs, so a silent child never
looks like a hang. Ask mode (`nevermore ask`) drains the same jobs while
its turn streams — the blocking pump waits on the live round AND every
registered job — so a chatty background command is never left wedged
mid-write waiting for the next tool call to read it.

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
docs/               wire specs + platform notes
data/               static model catalogs
```

See [docs/PORTTY.md](docs/PORTTY.md) for the terminal-feature matrix.

## Attributions

- **stb** — `stb_image.h` and `stb_image_write.h` by Sean Barrett
  (public domain / Unlicense) decode and re-encode the image-transcode
  lane (a JPEG handed to a kitty-graphics terminal becomes a PNG).
  Nothing to install: they are single-file headers, fetched into
  `third_party/stb/` by `scripts/fetch-stb.sh` on the first
  `configure` (a pinned commit, so that first configure needs
  network), and never committed to the tree.

## License

MIT — see COPYING.
