# nevermore

**Homepage:** <https://chest.so/nevermore/>

An interactive coding agent in pure C.

nevermore chats with AI models (Charm Hyper, Ollama local daemon and
Ollama Cloud, OpenRouter, OpenCode Go and Zen) from a
terminal, with an agent loop that can read, edit, and search files,
run commands, and search the web through a local SearXNG instance. It
is a first-class citizen of the [portty](https://github.com/chestso/portty)
terminal: kitty keyboard protocol, sixel image attach — graceful
degradation elsewhere.

Provider names carry the endpoint tier explicitly: `ollama:cloud`,
`ollama:local`, `opencode:go`, `opencode:zen`. A bare noun or a dash
suffix reads as a separate service; the colon keeps the service and
the tier distinct.

Pre-alpha. No backwards-compatibility constraint.

## Documentation

- [<img src="https://img.shields.io/badge/Ask%20DeepWiki-blueviolet" alt="Ask DeepWiki" height="20" align="absmiddle">](https://deepwiki.com/chestso/nevermore) — AI-generated wiki and searchable code index for this repository
- [ARCHITECTURE.md](ARCHITECTURE.md) — what each module is, why it is shaped that way, and the principles behind it
- [docs/PORTTY.md](docs/PORTTY.md) — the terminal-feature matrix
- [CONTRIBUTING.md](CONTRIBUTING.md) — the contributor's checklist: gates, test conventions, and traps

## Download

The [releases](https://github.com/chestso/nevermore/releases) page carries a
portable ZIP for macOS (Apple Silicon) and one for Windows (x86_64): extract
the folder wherever you like and run `nevermore` / `nevermore.exe` from any
terminal. No installer, no MSYS2, no Homebrew — each ZIP is the binary plus
whatever its own import table names that its OS does not ship. On Windows
that is the MinGW runtime when the toolchain links it dynamically
(`libwinpthread-1.dll`, which boba's static archive needs for its mutexes,
`pthread_once` and thread keys); the MSYS2 UCRT64 build links it in
statically, so its ZIP carries the executable and the docs alone. macOS
needs nothing at all, because boba links statically and TLS is the system's
Secure Transport. Every ZIP also carries a `README-PORTABLE.txt` that says
where the two files it writes live.

GNU/Linux gets the source tarball and nothing else: the platform has the
toolchain and the packages (OpenSSL or mbedTLS) already, so building it is
the install path.

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

- **boba** (required) — TUI runtime, textinput, styles. A separate
  project, not vendored: build and install a checkout of it, then let
  pkg-config find it —
  `PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig:$HOME/.local/lib64/pkgconfig"`
  (the `configure` line above already sets it; `./configure` names the
  same path if `boba.pc` is not found).
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
nevermore -p opencode:go -m glm-5.3  # one run, ignoring the saved config
nevermore -i shot.png "what is wrong in this screenshot?"
```

`ask` and `models` are subcommands and come **first**: a bare prompt is
ask mode (`nevermore "explain this repo"` is the same as the verb
spelling), and a mode word anywhere else is a usage error rather than a
prompt — `nevermore ask` alone says the prompt is missing instead of
asking the model about the word "ask".

Provider keys come from the environment (`HYPER_API_KEY`,
`OLLAMA_API_KEY`, `OPENROUTER_API_KEY`,
`OPENCODE_API_KEY`) or, when an
env var is unset, from `~/.authinfo` — one `machine <name> password
<secret>` line per provider:

```
machine hyper.charm.land   login apikey password ...
machine ollama.com         login apikey password ...
machine openrouter.ai      login apikey password ...
machine opencode.ai        login apikey password ...
```

Environment variables win over the file. Point elsewhere with
`NEVERMORE_AUTHINFO=/path/to/authinfo`. `ollama:local` (the zero-config
default) needs no key — and reads none either: the local tier has no key
source at all, so an `OLLAMA_API_KEY` you exported for Ollama Cloud is
never attached to a local round (use `-p ollama:cloud` for the cloud).

Starting (or switching to) a provider that needs a key with none
configured says so up front — `export OPENROUTER_API_KEY`, or add the
`~/.authinfo` line — instead of failing on the first turn. The check
runs through the same key lookup as the request, so a `~/.authinfo`
entry counts.

On OpenRouter, nevermore identifies itself as an app: every chat
round carries `HTTP-Referer: https://chest.so/nevermore`, the display
name and the `cli-agent` category, so the traffic shows up as the
_nevermore_ app in OpenRouter's dashboard, rankings and marketplace
page (`openrouter.ai/apps?url=https://chest.so/nevermore`). Point
`NEVERMORE_APP_URL` somewhere else to attribute a run to a different
app — a fork, a private deployment, a CI rig that should not appear
in the public rankings. It replaces the URL, it does not rename the
app: a different URL is a different app on OpenRouter's side, so a
value that is not an absolute `http(s)` URL is refused (the shipped
one is used instead) rather than creating a junk app.

The input row is always on screen, busy or not: it gathers the next
prompt while a turn runs (keys edit it; Enter is a no-op until the turn
ends; Ctrl+C interrupts, and so does Esc — Esc cancels the round, a
running tool included, and does nothing else: your text stays, and the
app stays). Above the prompt its status row carries the
context gauge, `<used>/<limit> ↓<out> ⚡<pct>`: the used count is the
prompt the provider last reported and the limit the active model's
catalog window, `↓<out>` is the output the conversation has accumulated
(every completed round's `completion_tokens`, summed) — the row's one
Cyan field, the spec's own color for a marker, in whichever spelling the
terminal can show (see `color` below) — and `⚡<pct>` is
the session's cache rate — the share of this conversation's input that
came from the provider's prefix cache, accumulated over every completed
round that reported a read count (a round that omits the field is left
out of the rate, never counted as a miss). The write side is tracked
separately and not rated. An unknown number reads as `-`, never an
estimate, and the `↓` and `⚡` markers are simply absent until a round has
reported the fact behind them — a _reported_ zero is a report, so it
reads `↓0`. The row's
right end names the endpoint it is talking to — `provider · model` —
elided from the tail when the terminal is narrow, and dropped when there
is no room for it at all; what the turn is DOING is the spinner glyph at
the row's left — the green dot pulses while the model streams (blinking
on a terminal without 24-bit color), the star set rotates while a tool
runs — so there is no word beside it to go stale between states.
The row paints **foreground only** — no surface — so it sits on the
terminal's own background. That is deliberate and is the one departure
from the spec's UI Color Palette: chest.so's projects run terminals with
a pitch-black, often transparent background, and a painted surface shows
up as a rectangle under the chrome, the one rectangle that is supposed to
disappear into the terminal. The `─` that
fills the row's slack is the spec's **Current Line** token, the color
its UI guidelines give a subtle border or separator; the rest of the row
is foreground text on the canvas.
`/context` prints the exact breakdown — the last
round's numbers plus the session totals, the cache read (and its rate),
and the cache write count. Its output line is the same accessor the row's
`↓` reads, so the compact and spelled-out forms cannot disagree.

Providers differ in what they report: Hyper, OpenRouter and the
OpenCode tiers report a cache read (`prompt_tokens_details.cached_tokens`)
— Hyper's is occasionally absent for a round — the OpenCode upstreams
also report a cache write, and Ollama reports the token counts but no
cache breakdown. Every provider's usage object is folded into one
canonical set by the shared client; where a fact is not reported the
display degrades (no `↓` or `⚡`, `session cache: not reported`), it is
never invented.

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
   `NEVERMORE_MAX_ROUNDS`, `NEVERMORE_VOID_RETRIES`,
   `NEVERMORE_REASONING_ECHO`, `NEVERMORE_SIGNATURE_ECHO`,
   `NEVERMORE_TIMEOUT_MS`, `NEVERMORE_TEMPERATURE`,
   `NEVERMORE_MAX_TOKENS`, `NEVERMORE_CONNECT_TIMEOUT_MS`,
   `NEVERMORE_CONNECT_FAMILY_SKIP`, `NEVERMORE_CONNECT_SKIP_FAMILIES`,
   `NEVERMORE_SEARXNG_URL`, `NEVERMORE_SEARXNG_ENABLED`,
   `NEVERMORE_SEARXNG_TIMEOUT_MS`, `NEVERMORE_RUN_COMMAND_TIMEOUT_MS`,
   `NEVERMORE_SKILLS`, `NEVERMORE_IMAGE_LIVE`
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
provider         = opencode:go
model            = glm-5.3
rounds           = 40
void_retries     = 2
# The two echo knobs: unset, the copies ride back; a value here
# overrides that (see "the two echo knobs").
# reasoning_echo = tools
# signature_echo = tools
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
kbd              = auto
spinner_ramp     = on
prism_notice     = on
skills           = on
image_live       = 1
# These two have NO default: unset, the field is never sent and the
# provider's own value rules (what every build did before they existed).
# temperature    = 0.3
# max_tokens     = 8192
```

The value is the rest of the line, trimmed and taken verbatim — no
quoting, no inline comments. Unknown keys and invalid values warn and
are skipped, so a stale file can never break startup. No secrets: API
keys stay in the environment or `~/.authinfo`.

`model` is remembered **per provider**: a model id belongs to one
provider, so a scoped `model.<provider>` line (e.g. `model.opencode:go =
glm-5.3`) is what the chat writes when you pick a model, and it
outranks the plain `model`. `-m` and `$NEVERMORE_MODEL` stay global
(an explicit flag is not memory). With no model set for the active
provider, nevermore says so and waits for `/model` — ask mode exits
before any traffic — rather than guessing an id from another
provider's catalog.

`rounds` caps the tool rounds in one turn (default 25): a model that
keeps calling tools without ever answering is stopped with "too many
tool rounds without a final answer". `void_retries` (default 2, `0`-`9`)
caps a different thing — how many times a round that came back with
**nothing in it** is asked again. A void round is not a tool round: the
provider rejected the model's function call (`finish_reason: error`,
`native_finish_reason: MALFORMED_FUNCTION_CALL`), so no text and no tool
call arrived, and the same transcript is asked again with a note telling
the model what the provider rejected. Set it to `0` and the first empty
round ends the turn instead — the retry is a net, not a fix, and this is
how you make the failure visible rather than absorbed.

Either way, **every void round is a system error you will see**: red in
the chat, `void round: the model produced no output (finish_reason: …,
native: …) — retrying (1/2)` (or `— retries spent (2/2), the turn ends`,
or `— no retries configured (void_retries = 0), the turn ends`), and the
same line on stderr in ask mode. The reason is quoted from the wire, not
guessed: an empty round is never silent, which is exactly what the
2026-10-27 session showed before this existed — eight rounds that stopped
a turn with nothing said and nothing shown.

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

`temperature` (`$NEVERMORE_TEMPERATURE`) and `max_tokens`
(`$NEVERMORE_MAX_TOKENS`) are the request's two sampling knobs, and
**neither has a default**: unset, the field is not sent at all and the
provider's own value rules — which is exactly what every build did
before they existed. `/config` prints an unset one as `-`.

`temperature` is a decimal in the `0`–`2` range the OpenAI-compatible
gateways publish. It divides the model's logits before the softmax, so
it decides how often the third-most-likely token wins: `0` is greedy
decoding (the front-runner at every step), `1` is the model's own
distribution, `2` flattens it toward uniform. It is not a capability or
quality dial — past about 1.1 what you see is drifting tool arguments
and wandering prose, and a reasoning model samples over its own trace,
so there the knob moves the trace's verbosity, not its correctness. Its
honest use in an agent is reproducibility: `temperature = 0` turns a
flaky live repro into a mostly-stable one. A value outside the grammar
(or above `2`) is refused at the layer that set it, never silently
clamped.

`max_tokens` caps the output of one round; a positive decimal, the same
shape `rounds` uses (so the read clamps at 100000). A round that hits
the cap ends with `finish_reason: length`, which nevermore already
reports — the key only sets the limit. Both are resolved at the point of
use, so `/config set temperature 0` applies to the next round.

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

### What goes back on the wire: the two echo knobs

A reasoning model says two different things, and only one of them is
prose. Both are received, shown where they are meant to be shown, and
kept in the transcript; only what rides _back_ to the provider is a
choice, and there are two independent choices for the two carriers:

| carrier                                                                                              | where it arrives                                                        | knob                                           |
| ---------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------- | ---------------------------------------------- |
| **trace text** — `reasoning_content` / `reasoning`                                                   | reasoning stream; `### reasoning` in the transcript, dimmed in the chat | `reasoning_echo` / `$NEVERMORE_REASONING_ECHO` |
| **structured carrier** — `reasoning_details[]` (Gemini's thought signature, OpenAI's encrypted blob) | its own stream channel, assembled once per round; never displayed       | `signature_echo` / `$NEVERMORE_SIGNATURE_ECHO` |

Both knobs take the same three values, with the old bool spellings
folded in (`on`/`true`/`1` mean `all`, `off`/`false`/`0`/`no` mean
`off`):

- `off` — nothing goes back. Still received, still kept, still shown
  on screen and saved to the transcript.
- `tools` — every assistant message that carries `tool_calls` re-sends
  its round's copy. This is where both carriers matter: the upstream
  replay check (and Google's signature rule) is about the tool result.
- `all` — that, plus every other assistant message that has one. **This
  is the default**: unset, the copies ride back.

**The knob is the whole control.** There is no per-provider layer: the
two keys decide what goes back, on every wire alike, and the built-in
default is `all`. Your config, the shadow file, the environment and
the command line all feed the same key, and the highest one that sets
it wins — so `off` (or `tools`) at any layer is honoured.

Two wires make the echo non-optional, which is why the default is
`all` and not `off`:

- `opencode:go`'s `deepseek` endpoint returns an intermittent 400 when
  a replayed tool-call round omits `reasoning_content` (probed), so a
  `deepseek` model there stops failing on tool rounds.
- `openrouter`'s Gemini models mint a thought signature on every
  function-calling round, and the upstream documents the signature as
  required back _with_ the tool result (`missing thought_signature` is
  a 400 otherwise).

`/config` tells you what the next request will send, not what your file
says: a row reads `all` when nothing is set anywhere, the layer that
dictates it otherwise (`user config`, `env`, …), and `all (frozen this
chat)` once a conversation has actually carried the field.

**The signature echo is strict echo-back.** It only ever re-sends
elements the model itself sent in this conversation — never a
synthesized array, never an empty placeholder — so a model that never
returns `reasoning_details` never receives the field, whatever the knob
says. The trace echo is different by
design: on a tool-call message under `tools`/`all` it sends the field
even when the round streamed no trace (`"reasoning_content":""`),
because that replay check tests the field's _presence_, not its
content.

**Both freeze once sent, and they freeze separately.** The first request
that actually carries a trace (or a `reasoning_details` field) latches
that knob for the rest of the conversation: a request prefix that gains
or loses the field is a different prefix, so changing the knob
mid-conversation would throw the provider's prompt cache away (and, for
the trace echo, can re-trip the check the echo exists for). The change
is still stored — it applies to the next chat — and `/config` says so
when you try. Because the latches are separate, the trace echo being off
while the signature echo is on is a normal state, not a bug.

**What it costs.** Both echoes put the copies on _every_ later request
in the conversation (the whole transcript rides each request). The
signature array is the bigger one when a model sends visible reasoning
elements — measured on a 95-file tool session, `reasoning_details` was
8–11% of the request body at peak and grew with the transcript. It is
cacheable, so most of it is billed as a cache read, and the first
request that carries a copy re-bills the tail once, at the freeze. The
knobs are therefore how you turn the echo _off_ — if you are paying per
token on a long session and nothing is failing, `off` is the cheaper
answer; if a wire needs the copy (the two above), `off` is the price
of that 400.

To see exactly what was sent, `NEVERMORE_DEBUG_WIRE=1` records every
request body and stream event verbatim under
`$XDG_STATE_HOME/nevermore/wire/` — that dump is the human's window on
a field the transcript deliberately never shows.

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

`kbd` (`$NEVERMORE_KBD`, default `auto`) is whether the terminal's
kitty keyboard protocol may be requested. `auto` asks for it once the
startup probe proves the terminal speaks it, `on` requests it without
waiting for an answer (a terminal that supports it but answers late),
and `off` keeps the legacy encodings — the escape hatch for a terminal
whose implementation misbehaves. It buys an unambiguous Esc and
unambiguous Ctrl+keys, **Shift+Enter as a newline** (the terminal
reports it as `CSI 13;2u`, which no legacy encoding can express), and
the text a key produced, so capitals and IME input arrive as
themselves. A terminal that speaks only part of the protocol degrades
to what it grants: without the report-all flag Shift+Enter stays
unavailable and `Ctrl+J` inserts the newline, and without the
associated text a shifted letter is recovered as its capital. The
report-all flag also reports the modifier keys themselves; they are
functional keys, not text, so pressing Shift types nothing. It also
makes the terminal report the lock keys' state on every key event,
which nevermore masks off — Num Lock on must not turn Shift+Enter into
a plain Enter.
`/config` shows the value _and_ what the terminal answered
(`terminal: kitty protocol` / `terminal: legacy encodings`), since the
store alone cannot say whether the protocol is in use.

`color` (`$NEVERMORE_COLOR`, default `auto`) is which of Dracula's two
published tables the app paints from. The spec publishes both — its
Standard hexes and its ANSI palette — and `auto` picks between them from
the terminal's own answer: 24-bit where the environment advertises it
(`COLORTERM`, or a direct-color `TERM`), the ANSI slots otherwise. `16`
forces the slots and `rgb` forces 24-bit; `ansi`/`ansi16` and
`truecolor`/`24bit` are accepted spellings of the same two.

The two tables agree for every role the spec names in both, so the
16-color rendering is **exact, not an approximation**: `AnsiCyan` _is_
`#8BE9FD`, `AnsiBrightBlack` _is_ `#6272A4`, and so on — a terminal
running Dracula's own palette draws the identical color. Two roles have
no ANSI entry at all (the amber and the selection bar), and the app
borrows a slot for the first and uses **reverse video** for the second
rather than inventing a palette entry the spec does not give it. This
assumes the terminal's palette is Dracula's; on some other palette a
slot renders that palette's color instead, which is what asking for a
slot means.

`auto` is right almost always, and it is a hint rather than a verdict:
the de-facto signal is `COLORTERM`, and plenty of capable terminals
never set it — so a terminal that can show 24-bit but does not say so
resolves as 16-color, and `color = rgb` is the escape hatch. The startup
banner names the tier in force (`Color tier: rgb (24-bit)` /
`Color tier: 16 (ANSI palette)`), so the answer is never a guess.

`spinner_ramp` (a bool, default `on`) is the thinking dot's size ramp:
while the model streams, the glyph breathes through `· • ● •`, and
turning it `off` holds a single `●`. It changes nothing about the
color: on a 24-bit terminal the dot pulses between the terminal
background and Dracula's green — it fades out of the background and
back, rather than dipping to a dark green — and on a terminal without
truecolor (no `COLORTERM`, no direct-color `TERM`) it blinks instead:
the same animation, told in the color the terminal can actually show.
The star set that rotates while a tool runs is unaffected either way.
The `color` key above decides which of the two that is; the dot's own
green is the same Green in both, and only the animation differs.

That one tier governs **every** color the app paints, not just the dot:
the prompt, the popup (whose selected row is reverse video at 16
colors, since the spec gives Selection no ANSI slot), the status row's
gauge and identity, the tool panels and errors on the system stream,
and the markdown transcript's headings, code, quotes, tables and fence
tokens. Each role names one spec color and the tier picks its spelling,
so no role can be left 24-bit-only by accident.

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

`prism_notice` (a bool, default `on`) is the gate for Hyper's Prism
routing line. `prism` is a model **router**: ask it and the gateway picks
the upstream that actually answers, naming it in a response header (not in
the body, which keeps saying `model: "prism"`). So every round a routed
turn finishes prints, on the terminal only:

```
prism: routed to qwen3.8-flash (Qwen3.8-Flash)
```

and a savings figure rides the same line when the gateway reported one
(today it reports it only for a non-streamed round, so a streaming client
usually sees the model alone). The line is a **system** message — the same
stream the connect-walk notice uses: it joins neither the conversation nor
the saved transcript, so it cannot shift the request prefix or the
gateway's cached prefix, and `/session save` stays the conversation. It is
one line per routed round, because each tool round is its own request and
is routed on its own. Turn the key `off` for silence; the capture is not
gated with it, so `/context` still answers "which model answered the last
round" either way.

## Skills

A **skill** is a folder of instructions for one kind of task, in the open
[Agent Skills](https://agentskills.io) format: a directory holding a
`SKILL.md` whose YAML frontmatter declares a `name` and a `description`,
with the instructions in the markdown body and any bundled files
(`references/`, `scripts/`, `assets/`) beside it. The format is not
nevermore's own — several agents read the same folders — and neither are
the locations: nevermore scans the cross-client convention the format's
own client guide names, and no vendor directory.

```
~/.agents/skills/<skill-name>/SKILL.md          yours, every project
<dir>/.agents/skills/<skill-name>/SKILL.md      the project's, from the
                                                repo root down to the
                                                working directory
```

A project skill overrides a user skill of the same name, and within a
project the directory nearest the working directory wins — the same
precedence `AGENTS.md` already has. A `.claude/skills/` directory is not
read: to use a collection that lives there, move or link it into
`~/.agents/skills/`.

Discovery loads only the metadata. The system prompt gains a `<skills>`
block — one line per skill, name and description, capped at 8 KiB with
the truncation announced in the block — and nothing else: no body rides
the prompt, so the cached prefix is not churned. When a task matches a
description, the model calls `load_skill` with that exact name; the
result is the `SKILL.md` body (frontmatter stripped) and the skill's
directory, which is what the instructions' relative file references
resolve against. The body arrives as an ordinary tool result, so it shows
in the transcript like any other tool output, and nevermore runs nothing
on a skill's behalf: a skill that says "run `scripts/x.py`" reaches the
machine through `run_command` and its existing gates.

`/skills` lists what was found, the tier each came from, its path, and
every validation finding — a name that breaks the spec's grammar, a
description clamped to the spec's 1024-character bound, a skill that
overrode another. Entries the format refuses outright (no description,
frontmatter that does not parse) are named there too: a skill you cannot
see is a skill that seems to not exist.

If nothing was discovered the prompt carries no `<skills>` block and the
`load_skill` tool is not offered at all. `skills` (a bool, default `on`,
env `NEVERMORE_SKILLS`) turns discovery off entirely; the setting is read
when an agent is built, so a change lands on the next chat or
`/provider` switch.

## Images (vision models)

A turn may carry images. In the chat, `/image <path>` attaches one to
the NEXT message (repeat for several), `/image` lists what is pending,
and `/image -<n>` drops one; in ask mode, `-i <path>` (repeatable) does
the same for the one-shot prompt:

```
/image ~/shot.png
what is wrong in this screenshot?     # the pending image rides this turn
```

One command covers both directions: bare `/image` lists what is
pending, `/image <path>` attaches, `/image -<n>` drops one,
`/image list` names every image the conversation already holds, and
`/image save [n] [path]` writes one out to a file (see below).

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

`/image` refuses a file that is unreadable, is not a PNG/JPEG/GIF, or
is over 8 MiB — the cap bounds **one image**, not the conversation, and
a local refusal keeps the message yours instead of a provider error.
That is the only cap: an image the attach accepted renders, however
large, because the transcript does not second-guess a payload that is
already in the conversation.

### How long an image rides a request

An image is **not** re-sent on every request. It rides the request
that introduced it and the next `image_live - 1` requests; after that
it is replaced by a short, frozen text marker — `[image shot.png —
PNG 800x600, 42.0 KiB — attached earlier, not repeated]` — that says
what the image was and that it is not being repeated. The marker is
byte-stable, so the provider's prefix cache keeps the rest of the
conversation cached; the swap itself costs one re-bill of the tail.

The window is a `/config` key, `image_live` (default `1`, env
`NEVERMORE_IMAGE_LIVE`), and it counts **requests**, not turns: one
turn can compose dozens of requests (a tool loop), and a turn-based
window would keep every image live for the whole turn. Set it to `off`
to put every image back on every request (the old behaviour, for a
workflow that really wants the pixels every time); `0` is refused.

The model's own images follow the same rule. A generated picture that
has aged out loses its `images` array (the providers' message-level
array takes image parts only) and its marker rides the message's text
— so the model re-draws from the description instead of editing the
picture, which is the trade the age rule buys.

A tool-read image's marker says so: `call read_file again to see it`.
A user attachment's marker does not — the store keeps the bytes, not a
path, and there is no tool that could send them again.

The model's own `read_file` meets the same three containers, and a file
it can see is an image but the wire cannot take (a WebP, say) is named
as such — container, dimensions, size, and what would work — instead of
being reported as unreadable text. Attachable containers are PNG, JPEG
and GIF; anything else has to be converted first. A file that is not
text at all is named too (a PDF, a ZIP, an ELF binary, or plainly
"binary file") rather than reported as a UTF-8 problem.

Sending an image to a text-only model is not an error — the provider
strips it and the model answers blind — so the catalog's vision flag is
a **warning**, never a refusal: `/image` notes it, and `/model` notes
it when the conversation already carries images.

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

Keeping one is `/image save`:

```
/image save            # the newest image -> nevermore-image-<n>.<ext> here
/image save 3          # the image the caption called #3
/image save 3 ~/a.png  # ...where you say
/image list            # every image in the conversation, with its number
```

The numbers are the chat's image order (attachments and generated
images share it), and `/image save` writes the conversation's own bytes —
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

## The session transcript

The conversation lives in the session, and the session is what rides
every request — nevermore never trims it (no guessed token budget). You
can look at it, and keep it, from the chat:

```
/session           # the shape: messages by role, content size, images
/session list      # every message, numbered, with its size and notes
/session save      # the whole transcript as markdown -> nevermore-session.md
/session save ~/chat.md
```

`/session` counts message 0 as `system` — it is the assembled prompt,
so the first thing a chat's transcript holds is the prompt you never
typed. Before any turn has run there is no session at all, and the
command says exactly that. `/session list` numbers the messages in
transcript order and adds the facts a size cannot show (`tool_calls`,
`reasoning`, `images`), which is how a long chat is read back without
scrolling: find the message, then ask the model about it. `/session
save` writes the same transcript as markdown — a
`<!-- nevermore session: N messages -->` header (plus a
`session-format` version), then one `## <role>` section per message
carrying a `<!-- meta: … -->` line and its bodies, tool results under
`## tool (<name>)`. An explicit path is taken verbatim (spaces
included); bare, the deterministic `nevermore-session.md` lands in the
cwd. Saving an empty transcript is refused, never an empty file.

The saved document's structure is the transcript's, never the
content's. Prose — `user` and `assistant` bodies, what a person typed
or a model wrote — is markdown, and it is written live, so its
headings, lists and emphasis render as its author meant them. That
includes the round's **reasoning trace**, which a message carries as
its own labelled passage: `### reasoning`, then `### content` for the
answer, so the two never run together. The `system` prompt and every
`tool` result are the other kind of body: bytes we assembled (`<env>`,
`<project_context>`) or relayed verbatim (a file's contents, a diff, a
command's output). They are not markdown, and written live they would
_become_ the document — a `#` joins the outline, a diff's `- `/`+ `
lines render as bullets, an unbalanced code fence swallows everything
after it, a `<env>` opens an HTML block. So each rides a fenced block
whose fence is one backtick longer than the longest backtick run in the
body: opaque by construction, byte-exact (unlike escaping, which would
rewrite the transcript), and still one screen of markdown. Tool
arguments in the `<!-- tool_calls -->` comment get the same treatment
(`-->` is escaped as `--\u003e`, the same JSON value), so an edit whose
new text contains one cannot end the comment early.

The file is also **readable back**, so the labels are for the reader
and the `meta` line is for the program. A body is arbitrary bytes, so
no delimiter it contains may decide the structure: a trace that mentions
`### content`, a command's output with a `## user` line in it, an
unbalanced fence. Each body's exact byte count rides in `meta`, and a
reader consumes that many bytes without ever scanning inside a body —
the count is the boundary, and the labels cannot move it. `meta` also
carries the role, the tool facts, and the images the message held. The
images themselves are still not written (a data URL is megabytes), so a
loader must refuse a file that declares them rather than hand back a
transcript whose request prefix differs from the chat's.

That boundary rule is about bodies, and only bodies: the `## tool
(<name>) id=…` heading is the file's one uncounted line, and its name
and id are strings a provider sent, so the heading escapes their control
bytes as visible `\xNN` (a `\n` in an id would otherwise split the
heading and put a forged `<!-- meta: … -->` line exactly where a reader
looks for the real one). Nothing is lost — the heading is for a human,
and the tool facts are in `meta`.

Images are **not** in the markdown: a data URL is megabytes, and the
save is an inspection surface, not a backup format. `/image save <n>`
writes one image's own bytes. Loading a saved transcript back is a post-1.0
item (the writer is the only half that exists).

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
`exec_command`. Its result opens with the command's exit status
(`Process exited with code 3`), then the combined capture under
`Output:` — the same report shape `exec_command` prints, so a failing
command is visible without reading the body.

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
looks like a hang. Ask mode (`nevermore ask "prompt"`) drains the same
jobs while its turn streams — the blocking pump waits on the live round
AND every registered job — so a chatty background command is never left
wedged mid-write waiting for the next tool call to read it.

Jobs run on both platforms, over the shell each one's spawn uses
(`/bin/sh -c` on POSIX; `cmd.exe /d /c` on Windows, whose jobs are
pipes plus a Job Object for the group kill). They are process-global
and each occupies a slot in the event loop's I/O-source pool, so at
most `NM_PROC_MAX_JOBS` (31: the pool less the agent's own source)
run at once — one past that fails loudly rather than starting a child
nothing would read. They die with nevermore.

## Smoke testing without a provider

The test suite never needs a network. For a manual/visual pass,
`tools/fake-ollama/fake_ollama.py` is a stdlib fake provider (pair it
with `-p ollama:local`; `--scenario table-fence` exercises reasoning +
table + fence + prose in one turn), and `tools/wire-replay` replays a
captured wire dump byte-exact — each tool's README has the recipe.

## Layout

```
src/                the nevermore binary: entry point, config, providers,
                    wire client, transport, SSE, JSON, agent loop, tools,
                    skills, session, TUI (headers live alongside sources —
                    CLI app, no library)
tests/              standalone test binaries (RUN_TEST/TEST_SUMMARY pattern)
tools/wire-replay/  replay server for captured wire dumps (Python)
docs/               wire specs + platform notes
```

See [docs/PORTTY.md](docs/PORTTY.md) for the terminal-feature matrix,
and [ARCHITECTURE.md](ARCHITECTURE.md) for the module design and the
principles behind it.

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
