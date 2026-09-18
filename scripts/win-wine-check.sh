#!/bin/sh
# scripts/win-wine-check.sh - build + run the Windows test binaries
# under local Wine before pushing (MSYS2 CI does the real run).
#
# Prereqs: x86_64-w64-mingw32-gcc (cross toolchain), wine, and a
# configured build/ (../configure has run for the host build so
# nevermore_version.h + config.h exist).
#
# Exit: 0 = every binary built and passed under wine; 1 = a build or a
# run failed; 77 = a prereq is missing (no cross toolchain / no wine) —
# the suite's skip code, so a box without wine reads as "not run", not
# as sixteen failures.

set -e

cd "$(dirname "$0")/.."

CC=x86_64-w64-mingw32-gcc
command -v $CC >/dev/null 2>&1 || {
	echo "SKIP: $CC not found (mingw-w64 cross toolchain)"
	exit 77
}
command -v wine >/dev/null 2>&1 || {
	echo "SKIP: wine not found"
	exit 77
}

OUT=build/tests-wine
mkdir -p "$OUT"

# MinGW's libwinpthread DLL must sit next to the exes for Wine's loader.
PTHREAD_DLL=/usr/x86_64-w64-mingw32/sys-root/mingw/bin/libwinpthread-1.dll
[ -f "$PTHREAD_DLL" ] && cp "$PTHREAD_DLL" "$OUT/" 2>/dev/null

COMMON="-Wall -Wextra -Isrc -Itests -Ibuild -I."
TLS_SRC=src/tls_openssl.c # cross-compiled MinGW + OpenSSL; wine
# runs the schannel path via winemapped
# dlls only for the real backend tests

# Common core objects each test links (tests link sources directly).
run_build() {
	name=$1
	shift
	# Remove the target FIRST: a failed compile must not leave the
	# previous run's exe behind to "pass" under wine (that is how a
	# src/process.h shadowing <process.h> hid a broken Windows build).
	rm -f "$OUT/$name.exe"
	# shellcheck disable=SC2086
	$CC -o "$OUT/$name.exe" "$@" $COMMON 2>&1 | head -5
	[ -f "$OUT/$name.exe" ] || {
		echo "BUILD FAIL: $name"
		exit 1
	}
}

run_build test_sse tests/test_sse.c src/sse.c
run_build test_json tests/test_json.c src/json.c
run_build test_xxh3 tests/test_xxh3.c src/xxh3.c
run_build test_authinfo tests/test_authinfo.c src/authinfo.c
run_build test_config tests/test_config.c src/nm_config.c
run_build test_session tests/test_session.c src/session.c
run_build test_context tests/test_context.c src/context.c

run_build test_process tests/test_process.c src/nm_process.c src/nm_process_win.c

run_build test_tools tests/test_tools.c src/tools.c src/tools_file.c \
	src/tools_websearch.c src/tools_exec.c src/nm_process.c src/nm_process_win.c \
	src/tools_spawn_win.c src/os_compat_win.c \
	src/json.c src/transport.c src/transport_socket.c \
	src/wire_recorder.c $TLS_SRC -lws2_32 -lpthread

run_build test_web_search tests/test_web_search.c src/tools.c \
	src/tools_file.c src/tools_websearch.c src/tools_exec.c src/nm_process.c \
	src/nm_process_win.c src/tools_spawn_win.c \
	src/os_compat_win.c src/json.c src/transport.c \
	src/transport_socket.c src/wire_recorder.c $TLS_SRC \
	-lws2_32 -lpthread

run_build test_wire tests/test_wire.c src/transport.c src/transport_socket.c \
	src/wire_recorder.c src/json.c $TLS_SRC -lws2_32 -lpthread

run_build test_wire_recorder tests/test_wire_recorder.c src/wire_recorder.c \
	src/json.c src/transport.c src/transport_socket.c \
	$TLS_SRC -lws2_32 -lpthread

run_build test_provider tests/test_provider.c src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openai.c src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	src/openai_client.c src/json.c src/sse.c \
	src/transport.c src/transport_socket.c src/wire_recorder.c \
	$TLS_SRC \
	-lws2_32 -lsecur32 -lcrypt32

run_build test_source tests/test_source.c src/source.c src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openai.c src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	src/openai_client.c src/json.c src/sse.c \
	src/transport.c src/transport_socket.c src/wire_recorder.c \
	$TLS_SRC \
	-lws2_32 -lsecur32 -lcrypt32

run_build test_openai_client tests/test_openai_client.c src/openai_client.c \
	src/json.c src/sse.c src/transport.c \
	src/transport_socket.c src/wire_recorder.c $TLS_SRC src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openai.c src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	-lws2_32 -lpthread

run_build test_agent tests/test_agent.c src/agent.c src/session.c \
	src/context.c src/tools.c src/tools_file.c src/tools_websearch.c \
	src/tools_exec.c src/nm_process.c src/nm_process_win.c src/tools_spawn_win.c \
	src/os_compat_win.c src/json.c src/sse.c \
	src/openai_client.c src/transport.c \
	src/transport_socket.c src/wire_recorder.c $TLS_SRC src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openai.c src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	-lws2_32 -lpthread

run_build test_spinner tests/test_spinner.c src/spinner.c

run_build test_highlight tests/test_highlight.c src/nm_highlight.c

# test_history needs boba's textinput. No MinGW-built boba exists on
# this box (boba has no Windows CI of its own; nevermore's MSYS2
# workflow builds it natively at CI time). Wine can't link the Linux
# ELF boba, so this binary is CI-verified, not wine-verified —
# plain-model parts of history (escape/unescape) are exercised by
# test_history under Linux + macOS pre-flights.
echo "SKIP: test_history.exe (needs MinGW boba; MSYS2 CI covers it)"
echo "SKIP: test_markdown.exe (needs MinGW boba; MSYS2 CI covers it)"
echo "SKIP: test_chat_app.exe (needs MinGW boba; MSYS2 CI covers it)"

echo "=== running under wine ==="
fail=0
for exe in "$OUT"/*.exe; do
	name=$(basename "$exe")
	if WINEDEBUG=-all wine "$exe" >"$OUT/$name.log" 2>&1; then
		echo "PASS: $name"
	else
		echo "FAIL: $name (log: $OUT/$name.log)"
		fail=1
		cat "$OUT/$name.log"
	fi
done

exit $fail
