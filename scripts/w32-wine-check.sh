#!/bin/sh
# scripts/w32-wine-check.sh - build + run the Windows test binaries
# under local Wine before pushing (MSYS2 CI does the real run).
#
# Prereqs: x86_64-w64-mingw32-gcc (cross toolchain), wine, and a
# configured build/ (../configure has run for the host build so
# nevermore_version.h + config.h exist). A boba source tree is needed
# for the four boba-dependent tests ($BOBA_SRC, else ./boba, else
# ../boba): the script cross-builds boba's library itself, so the
# Windows half of a transcript/markdown change is verified HERE instead
# of waiting for MSYS2 CI. Without one, those four SKIP.
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
# (the last is for image_codec.c, the ONE TU that includes the
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
	# This run's roster (the wine loop below runs exactly these, never a
	# stale .exe an earlier checkout left behind).
	BUILT="$BUILT $name"
}

# ---------------------------------------------------------------------
# boba for Windows
# ---------------------------------------------------------------------
# Four tests link boba (test_history, test_markdown, test_chat_app,
# test_image). They used to be a bare SKIP here — "MSYS2 CI covers it" —
# which left the Windows half of every transcript/markdown change
# unverified on this box until CI ran. boba is a plain static library
# and its own configure cross-compiles, so build it out of tree with
# THIS script's toolchain and link those tests against it.
#
# Source tree: $BOBA_SRC, else ./boba (the layout CI checks out), else
# ../boba (the sibling on this box). No source tree => the four SKIP,
# exactly as they did before.
BOBA_SRC="${BOBA_SRC:-}"
if [ -z "$BOBA_SRC" ]; then
	for d in ./boba ../boba; do
		if [ -f "$d/src/Makefile.am" ]; then
			BOBA_SRC="$d"
			break
		fi
	done
fi
BOBA_LIB=""
BOBA_SRC_ABS=""
if [ -n "$BOBA_SRC" ] && [ -f "$BOBA_SRC/src/Makefile.am" ]; then
	BOBA_SRC_ABS="$(cd "$BOBA_SRC" && pwd)"
	BOBA_W32="$OUT/boba-w32"
	# boba's `configure` is autogen.sh's output (gitignored): generate it
	# when this checkout has never been configured.
	if [ ! -x "$BOBA_SRC_ABS/configure" ]; then
		echo "==> boba: ./autogen.sh (no configure in $BOBA_SRC)"
		(cd "$BOBA_SRC_ABS" && ./autogen.sh) \
			>"$OUT/boba-autogen.log" 2>&1 ||
			echo "WARN: boba autogen.sh failed (see $OUT/boba-autogen.log)"
	fi
	if [ -x "$BOBA_SRC_ABS/configure" ]; then
		mkdir -p "$BOBA_W32"
		# Out of tree, so boba's own working tree stays clean (its
		# .gitignore covers the generated files anyway). The logs are
		# evidence, like every other build here.
		if (cd "$BOBA_W32" &&
			"$BOBA_SRC_ABS/configure" --host=x86_64-w64-mingw32) \
			>"$OUT/boba-configure.log" 2>&1 &&
			make -C "$BOBA_W32/src" -j"$(nproc)" \
				>"$OUT/boba-make.log" 2>&1 &&
			[ -f "$BOBA_W32/src/libboba.a" ]; then
			BOBA_LIB="$BOBA_W32/src/libboba.a"
			# Which boba this run verified: the pinned SHA is what CI
			# uses, so say when they differ (an uncommitted boba change
			# is the one case this pre-flight is MOST wanted for).
			bo_sha="$(git -C "$BOBA_SRC_ABS" rev-parse --short HEAD \
				2>/dev/null || echo unknown)"
			pin_sha="$(sed -n 's/^ *BOBA_REF: *\([0-9a-f]*\)$/\1/p' \
				.github/workflows/windows.yml)"
			pin_short="$(printf '%s' "$pin_sha" | cut -c1-7)"
			echo "PASS: libboba.a (Windows, $BOBA_SRC @ $bo_sha)"
			if [ -n "$pin_short" ] && [ "$bo_sha" != "$pin_short" ]; then
				echo "NOTE: boba HEAD $bo_sha is not the pinned $pin_short"
				echo "      (the run verifies the tree on disk, not the pin)"
			fi
		else
			echo "WARN: boba Windows build failed (see $OUT/boba-configure.log,"
			echo "      $OUT/boba-make.log); the four boba tests will SKIP"
		fi
	fi
fi

# A boba-linking test: the Windows libboba.a from above, plus the
# socket library boba's runtime selects on (its own LDADD).
run_build_boba() {
	name=$1
	shift
	run_build "$name" "$@" -I"$BOBA_SRC_ABS/include" -L"$BOBA_W32/src" \
		-lboba -lws2_32
}
BUILT=""

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
# The remembered name-service answers: pure table + the fake clock (this
# binary owns time, so expiry is moved through, never waited out).
run_build test_addr_cache tests/test_addr_cache.c src/addr_cache.c \
	tests/fake_clock.c
run_build test_authinfo tests/test_authinfo.c src/authinfo.c
run_build test_config tests/test_config.c src/conf.c
run_build test_args tests/test_args.c src/args.c
run_build test_session tests/test_session.c src/session.c src/image_bytes.c src/size.c \
	src/json.c
run_build test_context tests/test_context.c src/context.c src/reminder.c \
	src/skills.c src/conf.c
run_build test_skills tests/test_skills.c src/skills.c src/conf.c

run_build test_file_ledger tests/test_file_ledger.c src/file_ledger.c

run_build test_proc tests/test_proc.c src/proc.c src/proc_w32.c

run_build test_tools tests/test_tools.c tests/fake_clock.c \
	src/tools.c src/tools_file.c \
	src/file_ledger.c src/xxh3.c src/skills.c \
	src/tools_websearch.c src/tools_exec.c src/proc.c src/proc_w32.c \
	src/tools_spawn_w32.c src/os_compat_w32.c \
	src/json.c src/transport.c src/transport_socket.c src/addr_cache.c \
	src/conf.c src/wire_recorder.c src/image_bytes.c src/size.c \
	$TLS_SRC -lws2_32 -lpthread

run_build test_web_search src/clock.c tests/test_web_search.c src/tools.c \
	src/tools_file.c src/tools_websearch.c src/tools_exec.c src/proc.c \
	src/file_ledger.c src/xxh3.c src/skills.c \
	src/proc_w32.c src/tools_spawn_w32.c \
	src/os_compat_w32.c src/json.c src/transport.c \
	src/transport_socket.c src/addr_cache.c src/conf.c src/wire_recorder.c \
	src/image_bytes.c src/size.c $TLS_SRC \
	-lws2_32 -lpthread

run_build test_wire src/clock.c tests/test_wire.c src/transport.c src/transport_socket.c src/addr_cache.c \
	src/conf.c src/wire_recorder.c src/json.c $TLS_SRC \
	-lws2_32 -lpthread

run_build test_wire_recorder src/clock.c tests/test_wire_recorder.c src/wire_recorder.c \
	src/json.c src/transport.c src/transport_socket.c src/addr_cache.c \
	src/conf.c $TLS_SRC -lws2_32 -lpthread

run_build test_provider src/clock.c tests/test_provider.c src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_openaicompat.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	src/openai_client.c src/json.c src/sse.c \
	src/transport.c src/transport_socket.c src/addr_cache.c src/conf.c \
	src/wire_recorder.c \
	$TLS_SRC \
	-lws2_32 -lsecur32 -lcrypt32

run_build test_source src/clock.c tests/test_source.c src/source.c src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_openaicompat.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	src/openai_client.c src/json.c src/sse.c \
	src/transport.c src/transport_socket.c src/addr_cache.c src/conf.c \
	src/wire_recorder.c \
	$TLS_SRC \
	-lws2_32 -lsecur32 -lcrypt32

run_build test_openai_client src/clock.c tests/test_openai_client.c src/openai_client.c \
	src/json.c src/sse.c src/transport.c \
	src/transport_socket.c src/addr_cache.c src/conf.c src/wire_recorder.c $TLS_SRC \
	src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_openaicompat.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	-lws2_32 -lpthread

run_build test_agent src/clock.c tests/test_agent.c src/agent.c src/session.c src/image_bytes.c src/size.c \
	src/context.c src/reminder.c src/skills.c src/file_ledger.c src/tools.c src/tools_file.c src/tools_websearch.c \
	src/tools_exec.c src/proc.c src/proc_w32.c src/tools_spawn_w32.c \
	src/os_compat_w32.c src/json.c src/sse.c \
	src/openai_client.c src/transport.c \
	src/transport_socket.c src/addr_cache.c src/conf.c src/wire_recorder.c $TLS_SRC \
	src/nevermore.c \
	src/authinfo.c src/conversation_id.c src/xxh3.c \
	src/provider_openaicompat.c \
	src/provider_hyper.c src/provider_ollama.c \
	src/provider_ollama_local.c \
	src/provider_openrouter.c \
	src/provider_opencode.c src/provider_opencode_zen.c \
	src/provider_test.c \
	-lws2_32 -lpthread

run_build test_spinner tests/test_spinner.c src/spinner.c

# The generated version header's fields (what the .rc above embeds):
# pure C, no boba. -Ibuild/src is the header's build dir.
run_build test_version tests/test_version.c -Ibuild/src

run_build test_highlight tests/test_highlight.c src/highlight.c

# The pixel half is pure C (no boba), so it cross-checks under wine.
run_build test_image_codec tests/test_image_codec.c src/image_codec.c \
	src/image_bytes.c src/size.c

# The four boba-dependent tests: linked against the Windows libboba.a
# the section above cross-built. test_chat_app is the whole app path
# (agent, providers, tools, session, chat_app) driven over a canned
# server, so it is the widest Windows pin this script can buy; the
# source lists mirror tests/Makefile.am's SOURCES for each binary.
if [ -n "$BOBA_LIB" ]; then
	run_build_boba test_history tests/test_history.c src/history.c
	run_build_boba test_markdown tests/test_markdown.c src/markdown.c \
		src/markdown_render.c src/highlight.c src/image.c \
		src/image_bytes.c src/image_codec.c src/size.c
	run_build_boba test_image tests/test_image.c src/image.c \
		src/image_bytes.c src/image_codec.c src/size.c src/markdown.c \
		src/markdown_render.c src/highlight.c
	run_build_boba test_chat_app tests/test_chat_app.c tests/fake_clock.c \
		src/chat_app.c src/conf.c src/spinner.c \
		src/sysout.c \
		src/markdown.c src/markdown_render.c src/highlight.c \
		src/image.c src/image_bytes.c src/image_codec.c src/size.c \
		src/agent.c src/conversation_id.c src/xxh3.c src/session.c \
		src/context.c src/reminder.c src/skills.c src/file_ledger.c src/tools.c \
		src/tools_file.c src/tools_websearch.c src/tools_exec.c \
		src/proc.c src/proc_w32.c src/tools_spawn_w32.c \
		src/os_compat_w32.c src/json.c src/sse.c src/openai_client.c \
		src/transport.c src/transport_socket.c src/addr_cache.c $TLS_SRC \
		src/nevermore.c src/authinfo.c src/source.c \
		src/provider_openaicompat.c src/provider_hyper.c \
		src/provider_ollama.c src/provider_ollama_local.c \
		src/provider_openrouter.c \
		src/provider_opencode.c src/provider_opencode_zen.c \
		src/provider_test.c \
		-lpthread
else
	BOBA_SKIP="needs a Windows boba (set BOBA_SRC, or check out ./boba or ../boba)"
	echo "SKIP: test_history.exe ($BOBA_SKIP)"
	echo "SKIP: test_markdown.exe ($BOBA_SKIP)"
	echo "SKIP: test_chat_app.exe ($BOBA_SKIP)"
	echo "SKIP: test_image.exe ($BOBA_SKIP)"
fi

echo "=== running under wine ==="
fail=0
# Only what THIS run built: a stale .exe from an earlier checkout (a
# renamed test) or from a build that failed before its link must not read
# as a pass — the hazard run_build's `rm -f` already guards at the build
# step, here at the run step.
for name in $BUILT; do
	exe="$OUT/$name.exe"
	if WINEDEBUG=-all wine "$exe" >"$OUT/$name.log" 2>&1; then
		echo "PASS: $name.exe"
	else
		echo "FAIL: $name.exe (log: $OUT/$name.log)"
		fail=1
		cat "$OUT/$name.log"
	fi
done

exit $fail
