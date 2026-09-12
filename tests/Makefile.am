# tests/Makefile.am - standalone test binaries (link sources directly,
# portty style — no library to link against)

check_PROGRAMS = \
    test_sse \
    test_json \
    test_provider \
    test_source \
    test_wire \
    test_openai_client \
    test_tls \
    test_session \
    test_tools \
    test_agent \
    test_spinner \
    test_history \
    test_chat_app

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_builddir) \
    $(BOBA_CFLAGS)

# Per-test runtime cap (house rule: loopback tests are ms-fast; a
# hang is a bug, not a condition to wait out). The watchdog wraps
# every test binary: NM_TEST_TIMEOUT seconds (default 10), then a
# stack snapshot into the .log and a core-dumping kill — so a hung
# test FAILS loudly with a diagnosable core in CI artifacts instead
# of stalling the workflow until the 4h job watchdog cancels it.
# Override for interactive debugging: NM_TEST_TIMEOUT=0 turns it off.
#
# HARNESS GOTCHA (found the hard way, 2026-09): without automake's
# `parallel-tests` option this project uses the SERIAL harness, and
# serial runs suffix-less binaries through LOG_COMPILER —
# TEST_LOG_COMPILER only ever applies to `.test`-suffix scripts.
# Setting only TEST_LOG_COMPILER wired the watchdog into a dead
# variable: it never ran, on any OS, which is why the macOS hangs
# burned 4h with the "watchdog" in place. LOG_COMPILER is the one
# that fires; TEST_LOG_COMPILER is kept for a future parallel-tests
# switch.
LOG_COMPILER = $(SHELL) $(top_srcdir)/scripts/test-watchdog.sh
TEST_LOG_COMPILER = $(SHELL) $(top_srcdir)/scripts/test-watchdog.sh

TEST_CFLAGS = -Wall -Wextra

# TLS backend source: same conditional as src/Makefile.am — exactly
# one backend .c, selected at configure time (each guards itself with
# its NM_TLS_* define). On macOS the Secure Transport backend needs
# -framework Security/CoreFoundation; Schannel needs secur32.
#
# automake gotcha: the variable must carry the FULL path and be the
# last element of each _SOURCES list — `../src/$(TEST_TLS_SOURCE)`
# (path prefix + variable) is silently dropped from the generated
# am_*_OBJECTS, so the backend never links. Verified with automake
# 1.18 minimal repro.
if TLS_SECTRANSPORT
TEST_TLS_SOURCE = ../src/tls_sectransport.c
endif
if TLS_SCHANNEL
TEST_TLS_SOURCE = ../src/tls_schannel.c
endif
if TLS_MBEDTLS
TEST_TLS_SOURCE = ../src/tls_mbedtls.c
TEST_TLS_LIBS = $(MBEDTLS_LIBS)
TEST_TLS_CFLAGS_EXTRA = $(MBEDTLS_CFLAGS)
endif
if TLS_OPENSSL
TEST_TLS_SOURCE = ../src/tls_openssl.c
TEST_TLS_LIBS = $(OPENSSL_LIBS)
TEST_TLS_CFLAGS_EXTRA = $(OPENSSL_CFLAGS)
endif

# Each test links only the sources it exercises.

# SSE parser (pure C, no transport/JSON dependency).
test_sse_SOURCES = test_sse.c ../src/sse.c
test_sse_CFLAGS = $(TEST_CFLAGS)

# JSON parser/builders (pure C).
test_json_SOURCES = test_json.c ../src/json.c
test_json_CFLAGS = $(TEST_CFLAGS)

# Provider registry (nevermore.c + the provider vtables; the ollama
# vtable calls into the shared openai_client, so it links too).
test_provider_SOURCES = test_provider.c ../src/nevermore.c \
    ../src/provider_hyper.c ../src/provider_ollama.c \
    ../src/provider_openai.c ../src/provider_openrouter.c \
    ../src/openai_client.c ../src/json.c ../src/sse.c \
    ../src/transport.c ../src/transport_socket.c $(TEST_TLS_SOURCE)
test_provider_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_provider_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_provider_LDADD = -lpthread $(TEST_TLS_LIBS)

# Listing-plane sources (registry + static catalog) — the picker's
# data seam. Same link set as test_provider (sources wrap providers).
test_source_SOURCES = test_source.c ../src/source.c ../src/nevermore.c \
    ../src/provider_hyper.c ../src/provider_ollama.c \
    ../src/provider_openai.c ../src/provider_openrouter.c \
    ../src/openai_client.c ../src/json.c ../src/sse.c \
    ../src/transport.c ../src/transport_socket.c $(TEST_TLS_SOURCE)
test_source_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_source_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_source_LDADD = $(TEST_TLS_LIBS)

# Transport wire tests (dummy threaded localhost servers, canned
# responses; loopback only, never real APIs). POSIX threads.
test_wire_SOURCES = test_wire.c ../src/transport.c ../src/transport_socket.c $(TEST_TLS_SOURCE)
test_wire_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_wire_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_wire_LDADD = -lpthread $(TEST_TLS_LIBS)

# End-to-end: the shared OpenAI-compatible client against a canned
# chat/completions SSE server (request validation + delta assembly).
test_openai_client_SOURCES = test_openai_client.c \
    ../src/openai_client.c ../src/json.c ../src/sse.c \
    ../src/transport.c ../src/transport_socket.c $(TEST_TLS_SOURCE) \
    ../src/nevermore.c ../src/provider_hyper.c ../src/provider_ollama.c \
    ../src/provider_openai.c ../src/provider_openrouter.c
test_openai_client_CFLAGS = $(TEST_CFLAGS) -I$(top_builddir)/src $(TEST_TLS_CFLAGS_EXTRA)
test_openai_client_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_openai_client_LDADD = -lpthread $(TEST_TLS_LIBS)

# TLS: negative handshake over loopback with the committed self-signed
# cert (system trust must reject it). Live probe is manual
# (NM_LIVE_TLS=1), never in make check.
test_tls_SOURCES = test_tls.c ../src/transport.c ../src/transport_socket.c \
    $(TEST_TLS_SOURCE)
test_tls_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_tls_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_tls_LDADD = -lpthread $(TEST_TLS_LIBS)

# Session: transcript + context window (pure C, links session.c).
test_session_SOURCES = test_session.c ../src/session.c
test_session_CFLAGS = $(TEST_CFLAGS)

# Tools: registry + schemas + file tools + spawn (canned temp files).
# Same spawn split as src/Makefile.am — one file per OS.
if HOST_WINDOWS
TOOLS_SPAWN_SOURCE = ../src/tools_spawn_win.c ../src/os_compat_win.c
else
TOOLS_SPAWN_SOURCE = ../src/tools_spawn_posix.c
endif
test_tools_SOURCES = test_tools.c ../src/tools.c ../src/tools_file.c \
    $(TOOLS_SPAWN_SOURCE) ../src/json.c
test_tools_CFLAGS = $(TEST_CFLAGS)
test_tools_LDADD = -lpthread

# Agent: the full stream -> tool -> stream loop against a canned
# loopback SSE server (the openai vtable with a test endpoint).
test_agent_SOURCES = test_agent.c ../src/agent.c ../src/session.c \
    ../src/tools.c ../src/tools_file.c $(TOOLS_SPAWN_SOURCE) \
    ../src/json.c ../src/sse.c ../src/openai_client.c \
    ../src/transport.c ../src/transport_socket.c $(TEST_TLS_SOURCE) \
    ../src/nevermore.c ../src/provider_hyper.c ../src/provider_ollama.c \
    ../src/provider_openai.c ../src/provider_openrouter.c
test_agent_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_agent_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_agent_LDADD = -lpthread $(TEST_TLS_LIBS)

# Spinner: pure model — frames per agent state (braille streaming,
# charset tool tier), cycle + stop semantics. No terminal I/O here;
# chat_app wires the ticks to boba's on_tick.
test_spinner_SOURCES = test_spinner.c ../src/spinner.c
test_spinner_CFLAGS = $(TEST_CFLAGS)

# History: escape/unescape + real save/load against a temp file via
# the path-override seam. Links boba's textinput (the history ring +
# Up/Down navigation are boba's; nevermore only persists them).
test_history_SOURCES = test_history.c ../src/history.c
test_history_CFLAGS = $(TEST_CFLAGS) $(BOBA_CFLAGS)
test_history_LDADD = $(BOBA_LIBS)

# Chat app: the inline TUI component, driven through boba without a
# terminal (runtime over tmpfile output, hand-fed messages) against
# canned SSE servers. The full source stack rides along: the app owns
# an agent (provider + openai_client + transport) and prints the
# transcript through tui_runtime_clear_inline (new in boba).
test_chat_app_SOURCES = test_chat_app.c ../src/chat_app.c ../src/spinner.c \
    ../src/agent.c ../src/session.c ../src/tools.c ../src/tools_file.c \
    $(TOOLS_SPAWN_SOURCE) ../src/json.c ../src/sse.c \
    ../src/openai_client.c ../src/transport.c ../src/transport_socket.c \
    $(TEST_TLS_SOURCE) ../src/nevermore.c ../src/provider_hyper.c \
    ../src/provider_ollama.c ../src/provider_openai.c \
    ../src/provider_openrouter.c
test_chat_app_CFLAGS = $(TEST_CFLAGS) $(BOBA_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_chat_app_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_chat_app_LDADD = $(BOBA_LIBS) -lpthread $(TEST_TLS_LIBS)

# Each test binary gets a matching TESTS entry so `make check` runs it.
TESTS = $(check_PROGRAMS)

# Build the test binaries WITHOUT running them. CI uses this in the
# parallel Build step (make -j ... check-programs) so the timed Run
# tests step is pure execution — `make check` in the serial harness
# would otherwise compile all 13 binaries one-by-one inside it.
# (There is no built-in `make check_PROGRAMS`: the serial harness
# only expands that variable inside its own recursive rule.)
.PHONY: check-programs
check-programs: $(check_PROGRAMS)

# Verbose logging flag convention: pass -v to any test binary.
TESTS_ENVIRONMENT =

EXTRA_DIST = test_helpers.h tls/test-cert.pem tls/test-key.pem tls/README.md

# Wire tests (dummy local servers, no real network) — separate target,
# like quoth's `make test-wire`.
check-wire:
	@echo "wire tests not yet implemented (phase 1)"
