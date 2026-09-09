#!/bin/sh
# scripts/win-wine-check.sh - build + run the Windows test binaries
# under local Wine before pushing (MSYS2 CI does the real run).
#
# Prereqs: x86_64-w64-mingw32-gcc (cross toolchain), wine, and a
# configured build/ (../configure has run for the host build so
# nevermore_version.h + config.h exist).

set -e

cd "$(dirname "$0")/.."

CC=x86_64-w64-mingw32-gcc
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
	# shellcheck disable=SC2086
	$CC -o "$OUT/$name.exe" "$@" $COMMON 2>&1 | head -5
	[ -f "$OUT/$name.exe" ] || {
		echo "BUILD FAIL: $name"
		exit 1
	}
}

run_build test_sse tests/test_sse.c src/sse.c
run_build test_json tests/test_json.c src/json.c
run_build test_session tests/test_session.c src/session.c

run_build test_tools tests/test_tools.c src/tools.c src/tools_file.c \
	src/tools_spawn_win.c src/os_compat_win.c src/json.c

run_build test_wire tests/test_wire.c src/transport.c src/transport_socket.c \
	$TLS_SRC -lws2_32 -lpthread

run_build test_provider tests/test_provider.c src/nevermore.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_openai.c src/provider_openrouter.c \
	src/openai_client.c src/json.c src/sse.c \
	src/transport.c src/transport_socket.c $TLS_SRC \
	-lws2_32 -lsecur32 -lcrypt32

run_build test_openai_client tests/test_openai_client.c src/openai_client.c \
	src/json.c src/sse.c src/transport.c \
	src/transport_socket.c $TLS_SRC src/nevermore.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_openai.c src/provider_openrouter.c \
	-lws2_32 -lpthread

run_build test_agent tests/test_agent.c src/agent.c src/session.c \
	src/tools.c src/tools_file.c src/tools_spawn_win.c \
	src/os_compat_win.c src/json.c src/sse.c \
	src/openai_client.c src/transport.c \
	src/transport_socket.c $TLS_SRC src/nevermore.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_openai.c src/provider_openrouter.c \
	-lws2_32 -lpthread

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
