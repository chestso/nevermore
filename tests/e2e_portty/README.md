# e2e_portty — the built binary through a real PTY (Phase 7)

`make check` runs every other suite by linking the sources directly: no
test ever execs `src/nevermore`. This directory closes that gap. It
drives the **built binary** through coffer's `cfr-debug` (a headless
PTY inspector: spawn a child on a PTY, pipe its output through coffer's
VT engine, assert on the rendered grid) with `tools/fake-ollama` as the
canned provider — startup, a turn end to end, the transcript's bytes,
and a clean exit, all through a real terminal.

The scenarios live in `*.script` (cfr-debug's script vocabulary);
`e2e-driver.sh` is the shared driver; each scenario gets a one-line
wrapper that is the automake `TESTS` entry.

## Running one

```sh
NM_E2E_NEVERMORE=$PWD/build/src/nevermore \
NM_E2E_TOOLS=$PWD/tools \
    tests/e2e_portty/e2e_turn.sh
```

Under `make check` the Makefile exports both variables
(`NM_E2E_NEVERMORE` = `$(abs_top_builddir)/src/nevermore`, never
`PATH`), so the same driver covers the release build, `build-asan`
(the ASan binary is what gets driven there), and `distcheck`.

## What the driver does

1. **Preconditions** (each failing one exits 77, the automake SKIP —
   see below): `python3`, `cfr-debug` on `PATH`, the built binary, and
   `cfr-debug --help-spec` naming every command the scenario uses.
2. **A hermetic sandbox**: `mktemp -d` becomes `HOME`, `XDG_CONFIG_HOME`
   and `XDG_STATE_HOME`; `NEVERMORE_AUTHINFO=/dev/null`. The user's
   config, history and keys are never touched.
3. **The canned provider** on an *ephemeral* port
   (`fake_ollama.py --host 127.0.0.1 --port 0`, whose startup line
   reports the real port), with `NEVERMORE_BASE_URL` pointing at the IP
   literal — no `localhost` resolution order to get wrong, and no
   collision with a real ollama on 11434. The port is polled for
   readiness before the app starts.
4. **The scenario**: `cfr-debug -r 28 -c 90 -w 0 -f <scenario>.script
   -o <raw> -- <binary> -p ollama:local -m fake:1b`. `-w 0` because the
   scenario's first `wait-for` is the startup gate.
5. **One wire-level pin**: the raw PTY capture must contain the focused
   prompt's pink (`38;2;255;121;198`). Everything else asserts on
   coffer's parse of the grid; this one proves the transcript's *bytes*
   end to end.

## The skip matrix (exit 77)

`make check` must never go red because a tool is missing from the box
or the runner. The driver skips, naming the reason, when:

| Missing | Message |
| --- | --- |
| `python3` | `python3 not found (fake-ollama is the canned provider)` |
| `cfr-debug` on `PATH` | `cfr-debug not installed (coffer's contrib PTY inspector)` |
| `--help-spec` lacks a command | `cfr-debug has no 'wait-for' command (rebuild coffer)` |
| the built binary | `nevermore binary not built at <path> (run make)` |

`cfr-debug` is a coffer tool, not a nevermore one: nevermore's CI checks
out boba but not coffer, so the e2e reports SKIP on all three workflows
until the CI-polish half of Phase 7 adds a coffer build. That is the
intended state, not a gap.

## Writing a scenario

Add `<name>.script` (cfr-debug's vocabulary: `wait-for`, `send`,
`raw`, `assert-contains`, `assert-not-contains`, `await-exit`,
`assert-exit`, `resize`, `render`), a one-line wrapper
`e2e_<name>.sh`, then the `TESTS` entry and the four `EXTRA_DIST`
files in `tests/Makefile.am`.

Two rules that come from the app, not from cfr-debug:

- **Every wait is a `wait-for` with an explicit budget**, bounded by the
  real clock. The budgets' SUM must stay under the watchdog's
  `NM_TEST_TIMEOUT` (60 s) — the driver is wrapped by
  `scripts/test-watchdog.sh` like any test binary.
- **Enter is a silent no-op while a turn is in flight** (`chat_app`'s
  type-ahead rule: the typed text stays in the input, nothing is
  submitted). A `wait-for` on the last visible delta therefore does NOT
  prove the turn is over — the stream's `[DONE]` can still be in
  flight. Type the command first, let the stream settle, then Enter,
  and send a second Enter as a cheap no-op retry (`turn.script` is the
  worked example). A blind `wait` before the first Enter is the wrong
  fix: it hides a real race behind a number.

## Files

| File | Purpose |
| --- | --- |
| `e2e-driver.sh` | the shared driver (not a `TESTS` entry itself) |
| `e2e_turn.sh` | scenario 1's `TESTS` entry |
| `turn.script` | scenario 1: boot, one `table-fence` turn, quit |
