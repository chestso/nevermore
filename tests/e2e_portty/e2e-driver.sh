#!/bin/sh
# e2e-driver.sh — run one scripted PTY scenario against the built binary.
#
# Usage: e2e-driver.sh SCENARIO [FAKE-OLLAMA-SCENARIO]
#
# Spawns the built nevermore through coffer's cfr-debug PTY inspector,
# points it at tools/fake-ollama on an ephemeral port, and replays
# tests/e2e_portty/<SCENARIO>.script against it. Exit 0 = the scenario
# passed, 1 = it failed, 77 = this box cannot run it (the automake skip
# convention; see README.md).
#
# TIMING DISCIPLINE: every wait in a scenario is a `wait-for` with an
# explicit budget, bounded by the real clock. The budgets are the only
# timing the scenario states, and their SUM must stay well under
# scripts/test-watchdog.sh's NM_TEST_TIMEOUT (60 s), which wraps this
# driver like any test binary.
set -u

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scenario=${1:-}
fake_scenario=${2:-table-fence}
if [ -z "$scenario" ]; then
	echo "usage: e2e-driver.sh SCENARIO [FAKE-OLLAMA-SCENARIO]" >&2
	exit 2
fi

skip() {
	echo "SKIP: $*" >&2
	exit 77
}

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# --- preconditions (77 = "this box cannot run the scenario") ---------
command -v python3 >/dev/null 2>&1 ||
	skip "python3 not found (fake-ollama is the canned provider)"

CFR_DEBUG=${CFR_DEBUG:-cfr-debug}
command -v "$CFR_DEBUG" >/dev/null 2>&1 ||
	skip "cfr-debug not installed (coffer's contrib PTY inspector)"

# The binary under test comes from the BUILD TREE (NM_E2E_NEVERMORE,
# exported by tests/Makefile.am) — never from PATH, so a stale installed
# nevermore can never be what the scenario proves.
NEVERMORE_BIN=${NM_E2E_NEVERMORE:-$dir/../../src/nevermore}
[ -x "$NEVERMORE_BIN" ] ||
	skip "nevermore binary not built at $NEVERMORE_BIN (run make)"

script=$dir/$scenario.script
[ -f "$script" ] || fail "no such scenario: $script"

# Feature detection: the installed cfr-debug must know the commands the
# scenario uses, else 77 naming the reason instead of a mystery failure.
spec=$("$CFR_DEBUG" --help-spec 2>/dev/null) ||
	skip "cfr-debug --help-spec failed"
for c in wait-for await-exit assert-exit; do
	case "$spec" in
	*"$(printf 'CMD\t%s' "$c")"*) ;;
	*) skip "cfr-debug has no '$c' command (rebuild coffer)" ;;
	esac
done

# --- hermetic sandbox ------------------------------------------------
tmp=$(mktemp -d) || exit 1
server_pid=
cleanup() {
	if [ -n "$server_pid" ]; then
		kill "$server_pid" 2>/dev/null
		wait "$server_pid" 2>/dev/null
	fi
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

# The user's config, state dir and keys are never touched: the scenario
# runs in a throwaway home with an authinfo of /dev/null (ollama:local
# is keyless anyway).
HOME=$tmp
XDG_CONFIG_HOME=$tmp/config
XDG_STATE_HOME=$tmp/state
NEVERMORE_AUTHINFO=/dev/null
export HOME XDG_CONFIG_HOME XDG_STATE_HOME NEVERMORE_AUTHINFO

# --- the canned provider on an ephemeral port ------------------------
tools=${NM_E2E_TOOLS:-$dir/../../tools}
python3 "$tools/fake-ollama/fake_ollama.py" --host 127.0.0.1 --port 0 \
	--scenario "$fake_scenario" >"$tmp/server.log" 2>&1 &
server_pid=$!

port=
i=0
while [ "$i" -lt 100 ]; do
	port=$(sed -n 's/^fake-ollama: 127\.0\.0\.1:\([0-9][0-9]*\),.*/\1/p' \
		"$tmp/server.log" 2>/dev/null | head -1)
	[ -n "$port" ] && break
	kill -0 "$server_pid" 2>/dev/null || break
	sleep 0.05
	i=$((i + 1))
done
[ -n "$port" ] || fail "fake-ollama never reported a port (see $tmp/server.log)"

# Readiness: the port is bound, but be sure the server answers before
# the app's first request. Real-clock bounded.
python3 - "$port" <<'PY' || fail "fake-ollama never answered /api/tags"
import sys, time, urllib.request
port = sys.argv[1]
deadline = time.monotonic() + 5
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(
            f"http://127.0.0.1:{port}/api/tags", timeout=0.5
        ) as r:
            if r.status == 200:
                sys.exit(0)
    except Exception:
        time.sleep(0.05)
sys.exit(1)
PY

# The IP literal (not `localhost`) is deliberate: one stack, one
# address, no resolution order to get wrong.
NEVERMORE_BASE_URL="http://127.0.0.1:$port/v1"
export NEVERMORE_BASE_URL

# --- run the scenario ------------------------------------------------
raw=$tmp/raw
"$CFR_DEBUG" -r 28 -c 90 -w 0 -f "$script" -o "$raw" \
	-- "$NEVERMORE_BIN" -p ollama:local -m fake:1b
rc=$?
if [ "$rc" -ne 0 ]; then
	echo "--- server log ---" >&2
	cat "$tmp/server.log" >&2
	fail "$scenario: cfr-debug exited $rc"
fi

# One wire-level pin: the focused prompt's pink (colors.h's app accent)
# must have reached the terminal, proving the transcript's BYTES end to
# end. Everything else asserts on coffer's parse of the grid.
grep -q '38;2;255;121;198' "$raw" ||
	fail "$scenario: the prompt's accent SGR never reached the terminal"

echo "PASS: $scenario ($fake_scenario)" >&2
exit 0
