#!/bin/sh
# test-watchdog.sh - per-test runtime cap for make check
#
# Wired in as automake's TEST_LOG_COMPILER: every test binary runs
# with a background watchdog. If the test exceeds NM_TEST_TIMEOUT
# seconds (default 10 — loopback tests are ms-fast; a hang is a
# bug), the watchdog:
#
#   1. snapshots the stack into the test's .log (lldb/gdb/sample,
#      best-effort — whatever exists on the runner),
#   2. raises the core limit (inherited) and kills with
#      core-dumping signals: QUIT, then ABRT, then KILL.
#
# so a hang FAILS loudly with the stack + a core dump in CI
# artifacts instead of stalling the whole workflow until the 4h
# job watchdog cancels it.
#
# The nonzero wait status (128+signal) makes test-driver report
# FAIL for a killed test. Passes through transparently otherwise.
#
# Never a polling loop in the app sense: this is test harness only.

secs=${NM_TEST_TIMEOUT:-10}
ulimit -c unlimited 2>/dev/null || true

"$@" &
pid=$!

# Watchdog: sleeps, then diagnoses and kills. Output joins the
# test's redirected stdout (test-driver appends it to the .log).
(
	sleep "$secs" 2>/dev/null
	if kill -0 "$pid" 2>/dev/null; then
		echo ""
		echo "=== WATCHDOG: test still running after ${secs}s: $* (pid $pid) ==="
		echo "=== WATCHDOG: stack snapshot, then core-dump kill ==="
		if command -v lldb >/dev/null 2>&1; then
			lldb -p "$pid" -b -o "thread backtrace all" -o "detach" \
				2>&1 || true
		fi
		if command -v gdb >/dev/null 2>&1; then
			gdb -p "$pid" -batch -ex "thread apply all bt" 2>&1 || true
		fi
		if command -v sample >/dev/null 2>&1; then
			sample "$pid" 1 2>&1 || true
		fi
		kill -QUIT "$pid" 2>/dev/null || true
		sleep 1 2>/dev/null
		kill -ABRT "$pid" 2>/dev/null || true
		sleep 1 2>/dev/null
		kill -KILL "$pid" 2>/dev/null || true
	fi
) &
wd=$!

wait "$pid"
st=$?
kill "$wd" 2>/dev/null || true
wait "$wd" 2>/dev/null || true
exit $st
