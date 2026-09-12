#!/bin/sh
# test-watchdog.sh - per-test runtime cap for make check
#
# Wired in as automake's TEST_LOG_COMPILER: every test binary runs
# with a background watchdog. If the test exceeds NM_TEST_TIMEOUT
# seconds (default 10 — loopback tests are ms-fast; a hang is a
# bug), the watchdog:
#
#   1. announces the hang into the test's .log (test-driver
#      redirects our stdout there),
#   2. grabs a stack snapshot — every diagnostic individually
#      time-boxed and SIGKILLed if it overstays. This is the macOS
#      CI lesson: `lldb -p` used to run BEFORE the kill chain, so
#      one stuck attach wedged the wrapper and stalled the whole
#      workflow for hours (Linux/Windows have no lldb and stayed
#      green — the asymmetry was the tell).
#   3. kills the test: QUIT, then ABRT, then KILL, each signal
#      followed by a bounded settle, so the chain ALWAYS
#      completes even if a signal is ignored or a diagnostic
#      left the target SIGSTOPped (we CONT after debuggers die),
#
# so a hang FAILS loudly with the stack in the .log (and a core
# where the OS drops one) in CI artifacts instead of stalling the
# workflow until the 4h job watchdog cancels it.
#
# A START/END pair with UTC timestamps also lands in
# NM_TEST_PROGRESS (default test-progress.log, tests build dir) —
# which test hung and for how long is answerable from artifacts
# even when the stack snapshot comes up empty.
#
# The nonzero wait status (128+signal) makes test-driver report
# FAIL for a killed test. Passes through transparently otherwise.
# NM_TEST_TIMEOUT=0 disables the cap for interactive debugging.
#
# Never a polling loop in the app sense: this is test harness only.

secs=${NM_TEST_TIMEOUT:-10}
progress=${NM_TEST_PROGRESS:-test-progress.log}

now() { date -u +%H:%M:%S; }

echo "$(now) START $*" >>"$progress"

# Run one diagnostic under a hard time box: if it does not exit on
# its own within $1 seconds it is SIGKILLed — a stuck lldb/sample
# must never delay the kill chain. (Killing an attached debugger
# leaves the target SIGSTOPped; callers CONT it afterwards.)
diag() {
	d_secs=$1
	shift
	"$@" &
	d_pid=$!
	d_left=$((d_secs * 10))
	while kill -0 "$d_pid" 2>/dev/null && [ "$d_left" -gt 0 ]; do
		sleep 0.1
		d_left=$((d_left - 1))
	done
	if kill -0 "$d_pid" 2>/dev/null; then
		echo "=== WATCHDOG: diagnostic still running after ${d_secs}s, SIGKILLing it ==="
		kill -KILL "$d_pid" 2>/dev/null || true
	fi
	wait "$d_pid" 2>/dev/null || true
}

ulimit -c unlimited 2>/dev/null || true

"$@" &
pid=$!

if [ "$secs" -gt 0 ]; then
	(
		sleep "$secs" 2>/dev/null
		if kill -0 "$pid" 2>/dev/null; then
			echo ""
			echo "=== WATCHDOG: test still running after ${secs}s: $* (pid $pid) ==="
			echo "=== WATCHDOG: stack snapshot (each boxed), then kill ==="
			if command -v sample >/dev/null 2>&1; then
				diag 5 sample "$pid" 1 2>&1 || true
				kill -CONT "$pid" 2>/dev/null || true
			fi
			if command -v lldb >/dev/null 2>&1; then
				diag 8 lldb -p "$pid" -b \
					-o "thread backtrace all" -o "detach" 2>&1 || true
				kill -CONT "$pid" 2>/dev/null || true
			fi
			if command -v gdb >/dev/null 2>&1; then
				diag 8 gdb -p "$pid" -batch -ex "thread apply all bt" 2>&1 || true
				kill -CONT "$pid" 2>/dev/null || true
			fi
			# Kill chain: each signal, then a bounded settle, so the
			# chain completes even if a signal is ignored.
			for sig in QUIT ABRT; do
				kill -"$sig" "$pid" 2>/dev/null || true
				i=0
				while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 20 ]; do
					sleep 0.1
					i=$((i + 1))
				done
				kill -0 "$pid" 2>/dev/null || break
			done
			kill -KILL "$pid" 2>/dev/null || true
			echo "=== WATCHDOG: end of watchdog for pid $pid ==="
		fi
	) &
	wd=$!
fi

wait "$pid"
st=$?
if [ -n "$wd" ]; then
	kill "$wd" 2>/dev/null || true
	wait "$wd" 2>/dev/null || true
fi
echo "$(now) END status=$st $*" >>"$progress"
exit $st
