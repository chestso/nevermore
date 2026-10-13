#!/bin/sh
# scripts/w32-wine-check.sh - build + run the Windows test binaries
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

COMMON="-Wall -Wextra -Isrc -Itests -Ibuild -I. -Ithird_party/stb"
# (the last is for nm_image_codec.c, the ONE TU that includes the
# vendored stb headers — the same mirror the macOS script needed)
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
	# Capture the compiler's output to a file; never pipe it straight
	# into `head`. head exits after its line count and SIGPIPEs the
	# compiler, killing it before the link — so a TU whose build emits
	# five warning lines "fails" with its real error line unprinted
	# (transport_socket.c's -Wformat-truncation warnings did exactly
	# that to test_tools).
	buildlog="$OUT/$name.build.log"
	# shellcheck disable=SC2086
	if ! $CC -o "$OUT/$name.exe" "$@" $COMMON >"$buildlog" 2>&1; then
		echo "BUILD FAIL: $name"
		cat "$buildlog"
		exit 1
	fi
	[ -f "$OUT/$name.exe" ] || {
		# rc was 0 but no artifact: never let a stale/missing exe read
		# as a pass under wine.
		echo "BUILD FAIL: $name (no $name.exe produced)"
		cat "$buildlog"
		exit 1
	}
	# Warnings still surface (the first few), now safely off a file.
	head -5 "$buildlog"
}

# The Windows resource script (the .exe's icon + version metadata) is
# compiled only by MSYS2 CI, so compile it here too: a broken include
# path or macro name in src/nevermore.rc is otherwise invisible until
# CI. Its version fields come from the build-generated
# nevermore_version.h, so the resource must carry THIS tree's version —
# a hand-kept number in the .rc is exactly what the grep catches.
WR=x86_64-w64-mingw32-windres
[ -f build/src/nevermore_version.h ] ||
	make -C build/src nevermore_version.h >/dev/null 2>&1 || true
rm -f "$OUT/nevermore-res.o"
if ! $WR --define _UNICODE --define UNICODE \
	-Ibuild/src -I. -Isrc src/nevermore.rc -O coff \
	-o "$OUT/nevermore-res.o" >"$OUT/nevermore-res.build.log" 2>&1; then
	echo "BUILD FAIL: nevermore-res.o (src/nevermore.rc)"
	cat "$OUT/nevermore-res.build.log"
	exit 1
fi
version=$(sed -n 's/^#define NEVERMORE_VERSION "\(.*\)"$/\1/p' \
	build/src/nevermore_version.h)
if command -v strings >/dev/null 2>&1 && [ -n "$version" ]; then
	# Into a file, not a pipe: grep -q exits on the first match, and the
	# early close would SIGPIPE strings (the same trap as a compiler
	# piped into head) — plus the dump is evidence to read.
	strings -el "$OUT/nevermore-res.o" >"$OUT/nevermore-res.strings"
	if grep -qF "$version" "$OUT/nevermore-res.strings"; then
		echo "PASS: nevermore-res.o (version $version)"
	else
		echo "FAIL: nevermore-res.o does not carry version $version"
		exit 1
	fi
else
	echo "SKIP: nevermore-res.o version grep (no strings, or no header)"
fi

run_build test_sse tests/test_sse.c src/sse.c
run_build test_json tests/test_json.c src/json.c
run_build test_xxh3 tests/test_xxh3.c src/xxh3.c
run_build test_authinfo tests/test_authinfo.c src/authinfo.c
run_build test_config tests/test_config.c src/conf.c
run_build test_args tests/test_args.c src/args.c
run_build test_session tests/test_session.c src/session.c src/nm_image_bytes.c src/nm_size.c
run_build test_context tests/test_context.c src/context.c src/nm_reminder.c

run_build test_file_ledger tests/test_file_ledger.c src/nm_file_ledger.c

run_build test_proc tests/test_proc.c src/proc.c src/proc_w32.c

run_build test_tools tests/test_tools.c tests/fake_clock.c \
	src/tools.c src/tools_file.c \
	src/nm_file_ledger.c src/xxh3.c \
	src/tools_websearch.c src/tools_exec.c src/proc.c src/proc_w32.c \
	src/tools_spawn_w32.c src/os_compat_w32.c \
	src/json.c src/transport.c src/transport_socket.c \
	src/conf.c src/wire_recorder.c src/nm_image_bytes.c src/nm_size.c \
	$TLS_SRC -lws2_32 -lpthread

run_build test_web_search src/nm_clock.c tests/test_web_search.c src/tools.c \
	src/tools_file.c src/tools_websearch.c src/tools_exec.c src/proc.c \
	src/nm_file_ledger.c src/xxh3.c \
	src/proc_w32.c src/tools_spawn_w32.c \
	src/os_compat_w32.c src/json.c src/transport.c \
	src/transport_socket.c src/conf.c src/wire_recorder.c \
	src/nm_image_bytes.c src/nm_size.c $TLS_SRC \
	-lws2_32 -lpthread

run_build test_wire src/nm_clock.c tests/test_wire.c src/transport.c src/transport_socket.c \
	src/conf.c src/wire_recorder.c src/json.c $TLS_SRC \
	-lws2_32 -lpthread

run_build test_wire_recorder src/nm_clock.c tests/test_wire_recorder.c src/wire_recorder.c \
	src/json.c src/transport.c src/transport_socket.c \
	src/conf.c $TLS_SRC -lws2_32 -lpthread

run_build test_provider src/nm_clock.c tests/test_provider.c src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_openaicompat.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openai.c src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	src/openai_client.c src/json.c src/sse.c \
	src/transport.c src/transport_socket.c src/conf.c \
	src/wire_recorder.c \
	$TLS_SRC \
	-lws2_32 -lsecur32 -lcrypt32

run_build test_source src/nm_clock.c tests/test_source.c src/source.c src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_openaicompat.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openai.c src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	src/openai_client.c src/json.c src/sse.c \
	src/transport.c src/transport_socket.c src/conf.c \
	src/wire_recorder.c \
	$TLS_SRC \
	-lws2_32 -lsecur32 -lcrypt32

run_build test_openai_client src/nm_clock.c tests/test_openai_client.c src/openai_client.c \
	src/json.c src/sse.c src/transport.c \
	src/transport_socket.c src/conf.c src/wire_recorder.c $TLS_SRC \
	src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_openaicompat.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openai.c src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	-lws2_32 -lpthread

run_build test_agent src/nm_clock.c tests/test_agent.c src/agent.c src/session.c src/nm_image_bytes.c src/nm_size.c \
	src/context.c src/nm_reminder.c src/nm_file_ledger.c src/tools.c src/tools_file.c src/tools_websearch.c \
	src/tools_exec.c src/proc.c src/proc_w32.c src/tools_spawn_w32.c \
	src/os_compat_w32.c src/json.c src/sse.c \
	src/openai_client.c src/transport.c \
	src/transport_socket.c src/conf.c src/wire_recorder.c $TLS_SRC \
	src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_openaicompat.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openai.c src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	-lws2_32 -lpthread

run_build test_spinner tests/test_spinner.c src/spinner.c

# The generated version header's fields (what the .rc above embeds):
# pure C, no boba. -Ibuild/src is the header's build dir.
run_build test_version tests/test_version.c -Ibuild/src

run_build test_highlight tests/test_highlight.c src/nm_highlight.c

# The pixel half is pure C (no boba), so it cross-checks under wine.
run_build test_image_codec tests/test_image_codec.c src/nm_image_codec.c \
	src/nm_image_bytes.c src/nm_size.c

# test_history needs boba's textinput. No MinGW-built boba exists on
# this box (boba has no Windows CI of its own; nevermore's MSYS2
# workflow builds it natively at CI time). Wine can't link the Linux
# ELF boba, so this binary is CI-verified, not wine-verified —
# plain-model parts of history (escape/unescape) are exercised by
# test_history under Linux + macOS pre-flights.
echo "SKIP: test_history.exe (needs MinGW boba; MSYS2 CI covers it)"
echo "SKIP: test_markdown.exe (needs MinGW boba; MSYS2 CI covers it)"
echo "SKIP: test_chat_app.exe (needs MinGW boba; MSYS2 CI covers it)"
echo "SKIP: test_image.exe (needs MinGW boba; MSYS2 CI covers it)"

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
