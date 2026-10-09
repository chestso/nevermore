# tests/Makefile.am - standalone test binaries (link sources directly,
# portty style — no library to link against)

check_PROGRAMS = \
    test_sse \
    test_json \
    test_xxh3 \
    test_authinfo \
    test_config \
    test_provider \
    test_source \
    test_wire \
    test_wire_recorder \
    test_openai_client \
    test_tls \
    test_session \
    test_context \
    test_tools \
    test_web_search \
    test_process \
    test_agent \
    test_spinner \
    test_history \
    test_markdown \
    test_highlight \
    test_image \
    test_image_codec \
    test_reminder \
    test_file_ledger \
    test_chat_app

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_srcdir)/third_party/stb \
    -I$(top_builddir) \
    $(BOBA_CFLAGS)

# Per-test runtime cap (house rule: a hang is a bug, not a condition
# to wait out). The watchdog wraps every test binary: NM_TEST_TIMEOUT
# seconds (default 60), then a stack snapshot into the .log and a
# core-dumping kill — so a hung test FAILS loudly with a diagnosable
# core in CI artifacts instead of stalling the workflow until the 4h
# job watchdog cancels it.
# Override for interactive debugging: NM_TEST_TIMEOUT=0 turns it off.
#
# WHY 60 AND NOT 10 (2026-10-03): the cap is a HANG DETECTOR, so it
# must sit far above the binary's legitimate runtime — and on the
# MSYS2 runner it did not. The heaviest binary (test_chat_app, 73
# tests) runs 0.7 s here, 11 s there, and the gap is NOT the tests'
# sleeps: the suite's sleeps were removed (test_wire went 1.4 s -> 0.2 s
# here) and the Windows number did not move at all. It is per-test
# environment overhead (thread/socket setup on a slow runner), so the
# old 10 s was measuring the runner, not a hang — and killed
# test_chat_app twice. 60 s is ~5x the worst legitimate runtime
# observed, still bounded, and still loud.
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

# XXH3-64 (seed 0) against reference-generated vectors: pure C, no
# network, no deps. Pins the Hyper affinity header digest values.
test_xxh3_SOURCES = test_xxh3.c ../src/xxh3.c
test_xxh3_CFLAGS = $(TEST_CFLAGS)

# authinfo key lookup (pure C: stdio/stdlib/string only, no sockets).
test_authinfo_SOURCES = test_authinfo.c ../src/authinfo.c
test_authinfo_CFLAGS = $(TEST_CFLAGS)

# Central config (user file + runtime shadow layer): pure C, no boba,
# no network, no registry (the provider-name validator is a hook).
test_config_SOURCES = test_config.c ../src/nm_config.c
test_config_CFLAGS = $(TEST_CFLAGS)

# Provider registry (nevermore.c + the provider vtables; the ollama
# vtables call into the shared openai_client, so it links too).
test_provider_SOURCES = test_provider.c ../src/nm_clock.c ../src/nm_config.c \
    ../src/nevermore.c \
    ../src/authinfo.c \
    ../src/conversation_id.c ../src/xxh3.c \
    ../src/provider_hyper.c ../src/provider_ollama.c \
    ../src/provider_ollama_local.c \
    ../src/provider_openai.c ../src/provider_openrouter.c \
    ../src/provider_opencode.c ../src/provider_opencode_zen.c \
    ../src/provider_test.c \
    ../src/openai_client.c ../src/json.c ../src/sse.c \
    ../src/transport.c ../src/transport_socket.c ../src/wire_recorder.c $(TEST_TLS_SOURCE)
test_provider_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_provider_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_provider_LDADD = -lpthread $(TEST_TLS_LIBS)

# Listing-plane sources (registry + static catalog) — the picker's
# data seam. Same link set as test_provider (sources wrap providers).
test_source_SOURCES = test_source.c ../src/nm_clock.c ../src/source.c ../src/nm_config.c \
    ../src/nevermore.c \
    ../src/authinfo.c \
    ../src/conversation_id.c ../src/xxh3.c \
    ../src/provider_hyper.c ../src/provider_ollama.c \
    ../src/provider_ollama_local.c \
    ../src/provider_openai.c ../src/provider_openrouter.c \
    ../src/provider_opencode.c ../src/provider_opencode_zen.c \
    ../src/provider_test.c \
    ../src/openai_client.c ../src/json.c ../src/sse.c \
    ../src/transport.c ../src/transport_socket.c ../src/wire_recorder.c $(TEST_TLS_SOURCE)
test_source_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_source_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_source_LDADD = $(TEST_TLS_LIBS)

# Transport wire tests (dummy threaded localhost servers, canned
# responses; loopback only, never real APIs). POSIX threads.
test_wire_SOURCES = test_wire.c ../src/nm_clock.c ../src/transport.c ../src/transport_socket.c \
    ../src/nm_config.c \
    ../src/wire_recorder.c ../src/json.c $(TEST_TLS_SOURCE)
test_wire_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_wire_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_wire_LDADD = -lpthread $(TEST_TLS_LIBS)

# Wire debug recorder (docs/WIRE-DEBUG.md): banner + NDJSON event
# lines + redaction via the header secret marker, plus a tap round
# trip against a canned loopback server. Direct recorder calls, no
# real network.
test_wire_recorder_SOURCES = test_wire_recorder.c ../src/nm_clock.c ../src/wire_recorder.c \
    ../src/json.c ../src/transport.c ../src/transport_socket.c \
    ../src/nm_config.c \
    $(TEST_TLS_SOURCE)
test_wire_recorder_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_wire_recorder_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_wire_recorder_LDADD = -lpthread $(TEST_TLS_LIBS)

# End-to-end: the shared OpenAI-compatible client against a canned
# chat/completions SSE server (request validation + delta assembly).
test_openai_client_SOURCES = test_openai_client.c ../src/nm_clock.c \
    ../src/openai_client.c ../src/json.c ../src/sse.c \
    ../src/transport.c ../src/transport_socket.c ../src/nm_config.c ../src/wire_recorder.c $(TEST_TLS_SOURCE) \
    ../src/nevermore.c ../src/authinfo.c ../src/conversation_id.c ../src/xxh3.c \
    ../src/provider_hyper.c \
    ../src/provider_ollama.c ../src/provider_ollama_local.c \
    ../src/provider_openai.c ../src/provider_openrouter.c \
    ../src/provider_opencode.c ../src/provider_opencode_zen.c \
    ../src/provider_test.c
test_openai_client_CFLAGS = $(TEST_CFLAGS) -I$(top_builddir)/src $(TEST_TLS_CFLAGS_EXTRA)
test_openai_client_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_openai_client_LDADD = -lpthread $(TEST_TLS_LIBS)

# TLS: negative handshake over loopback with the committed self-signed
# cert (system trust must reject it). Live probe is manual
# (NM_LIVE_TLS=1), never in make check.
test_tls_SOURCES = test_tls.c ../src/nm_clock.c ../src/transport.c ../src/transport_socket.c \
    ../src/nm_config.c \
    $(TEST_TLS_SOURCE)
test_tls_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_tls_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_tls_LDADD = -lpthread $(TEST_TLS_LIBS)

# Session: transcript + context window (pure C, links session.c +
# the image byte half, which the attach path shares with the display
# path — nm_image_bytes.c is boba-free precisely so this stays so).
test_session_SOURCES = test_session.c ../src/session.c ../src/nm_image_bytes.c ../src/nm_size.c
test_session_CFLAGS = $(TEST_CFLAGS)

# Context: AGENTS.md discovery + system-prompt assembly (pure C,
# links context.c only — no network, no boba).
test_context_SOURCES = test_context.c ../src/context.c ../src/nm_reminder.c
test_context_CFLAGS = $(TEST_CFLAGS)

# Tools: registry + schemas + file tools + spawn (canned temp files).
# Same spawn split as src/Makefile.am — one file per OS.
if HOST_WINDOWS
TOOLS_SPAWN_SOURCE = ../src/tools_spawn_win.c ../src/os_compat_win.c
PROCESS_SOURCE = ../src/nm_process_win.c
else
TOOLS_SPAWN_SOURCE = ../src/tools_spawn_posix.c
PROCESS_SOURCE = ../src/nm_process_posix.c
endif
# web_search rides transport (the async HTTP step machine), so the
# tools binary links the wire stack and the TLS backend source; the
# spawn source is still needed for run_command's vtable.
test_tools_SOURCES = test_tools.c fake_clock.c ../src/tools.c ../src/tools_file.c \
    ../src/tools_websearch.c ../src/tools_exec.c \
    ../src/nm_file_ledger.c ../src/xxh3.c \
    ../src/nm_process.c $(PROCESS_SOURCE) \
    $(TOOLS_SPAWN_SOURCE) ../src/json.c \
    ../src/transport.c ../src/transport_socket.c ../src/nm_config.c \
    ../src/wire_recorder.c ../src/nm_image_bytes.c ../src/nm_size.c \
    $(TEST_TLS_SOURCE)
test_tools_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_tools_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_tools_LDADD = -lpthread $(TEST_TLS_LIBS)

# web_search: the SearXNG HTTP tool against a canned loopback server —
# request shaping, result normalization (sort/dedup/cap), errors, the
# reachability cache and its timeout.
test_web_search_SOURCES = test_web_search.c ../src/nm_clock.c \
    ../src/tools.c ../src/tools_file.c ../src/tools_websearch.c \
    ../src/nm_file_ledger.c ../src/xxh3.c \
    ../src/tools_exec.c ../src/nm_process.c $(PROCESS_SOURCE) \
    $(TOOLS_SPAWN_SOURCE) ../src/json.c \
    ../src/transport.c ../src/transport_socket.c ../src/nm_config.c \
    ../src/wire_recorder.c ../src/nm_image_bytes.c ../src/nm_size.c \
    $(TEST_TLS_SOURCE)
test_web_search_CFLAGS = $(TEST_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_web_search_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_web_search_LDADD = -lpthread $(TEST_TLS_LIBS)

# Process jobs: the PTY registry, bounded buffer, dumb-terminal
# renderer, stdin writes, and group-kill (loopback-free; the OS seam is
# exercised for real on POSIX, and reports unsupported on Windows).
test_process_SOURCES = test_process.c ../src/nm_process.c $(PROCESS_SOURCE)
test_process_CFLAGS = $(TEST_CFLAGS)

# Agent: the full stream -> tool -> stream loop against a canned
# loopback SSE server (the openai vtable with a test endpoint).
test_agent_SOURCES = test_agent.c ../src/nm_clock.c ../src/agent.c ../src/conversation_id.c ../src/xxh3.c ../src/session.c \
    ../src/nm_image_bytes.c ../src/nm_size.c \
    ../src/context.c ../src/nm_reminder.c \
    ../src/nm_file_ledger.c \
    ../src/tools.c ../src/tools_file.c ../src/tools_websearch.c \
    ../src/tools_exec.c ../src/nm_process.c $(PROCESS_SOURCE) \
    $(TOOLS_SPAWN_SOURCE) \
    ../src/json.c ../src/sse.c ../src/openai_client.c \
    ../src/transport.c ../src/transport_socket.c ../src/nm_config.c ../src/wire_recorder.c $(TEST_TLS_SOURCE) \
    ../src/nevermore.c ../src/authinfo.c ../src/provider_hyper.c \
    ../src/provider_ollama.c ../src/provider_ollama_local.c \
    ../src/provider_openai.c ../src/provider_openrouter.c \
    ../src/provider_opencode.c ../src/provider_opencode_zen.c \
    ../src/provider_test.c
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

# Markdown: nevermore's line classifier (pure C, no I/O) + the boba
# transcript integration (classifier verdicts -> commit behavior).
# Links boba because the renderer writes through a TuiRowSink; the
# renderer's fence path calls the highlighter, so nm_highlight.c links
# too.
test_markdown_SOURCES = test_markdown.c ../src/nm_markdown.c \
    ../src/nm_markdown_render.c ../src/nm_highlight.c ../src/nm_image.c \
    ../src/nm_image_bytes.c ../src/nm_image_codec.c ../src/nm_size.c
test_markdown_CFLAGS = $(TEST_CFLAGS) $(BOBA_CFLAGS)
test_markdown_LDADD = $(BOBA_LIBS)

# Image tier: the policy half (src/nm_image.c) + integration through
# boba's transcript with the real renderer pair. Links boba (the
# TuiRowSink/transport path) and the markdown grammar (the image line
# is classified + parsed by nm_markdown).
test_image_SOURCES = test_image.c ../src/nm_image.c \
    ../src/nm_image_bytes.c ../src/nm_image_codec.c ../src/nm_size.c \
    ../src/nm_markdown.c ../src/nm_markdown_render.c ../src/nm_highlight.c
test_image_CFLAGS = $(TEST_CFLAGS) $(BOBA_CFLAGS)
test_image_LDADD = $(BOBA_LIBS)

# Image codec: the vendored stb decode/encode wrapper (src/nm_image_codec.c).
# Pure C, NO boba — it links the codec + the byte half only, which is
# the point (the codec must be usable without boba, exactly as
# nm_image_bytes is for the session/agent tests).
test_image_codec_SOURCES = test_image_codec.c ../src/nm_image_codec.c \
    ../src/nm_image_bytes.c ../src/nm_size.c
test_image_codec_CFLAGS = $(TEST_CFLAGS)

# Reminders: the tag framing, the trust boundary (sanitizer) and the
# rule table — pure C (no boba, no I/O, no config store).
test_reminder_SOURCES = test_reminder.c ../src/nm_reminder.c
test_reminder_CFLAGS = $(TEST_CFLAGS)

# File ledger: the per-session read memory behind `file-already-read` /
# `file-changed` — pure C (no I/O, no boba, no config store: the tool
# hands the facts in).
test_file_ledger_SOURCES = test_file_ledger.c ../src/nm_file_ledger.c
test_file_ledger_CFLAGS = $(TEST_CFLAGS)

# Fence token highlighter: pure C (no boba, no I/O) — the resumable
# per-line tokenizer behind the labeled-fence body styling (IR step
# 4b). Unit-tests every language's token classes, cross-line block
# comment state, and language detection.
test_highlight_SOURCES = test_highlight.c ../src/nm_highlight.c
test_highlight_CFLAGS = $(TEST_CFLAGS)

# Chat app: the inline TUI component, driven through boba without a
# terminal (runtime over tmpfile output, hand-fed messages) against
# canned SSE servers. The full source stack rides along: the app owns
# an agent (provider + openai_client + transport) and drives the
# transcript through boba's streaming IR (src/chat_app.c).
test_chat_app_SOURCES = test_chat_app.c fake_clock.c ../src/chat_app.c ../src/nm_config.c \
    ../src/spinner.c \
    ../src/nm_markdown.c ../src/nm_markdown_render.c ../src/nm_highlight.c \
    ../src/nm_image.c ../src/nm_image_bytes.c ../src/nm_image_codec.c ../src/nm_size.c ../src/agent.c ../src/conversation_id.c ../src/xxh3.c ../src/session.c ../src/context.c ../src/nm_reminder.c ../src/nm_file_ledger.c ../src/tools.c ../src/tools_file.c ../src/tools_websearch.c \
    ../src/tools_exec.c ../src/nm_process.c $(PROCESS_SOURCE) \
    $(TOOLS_SPAWN_SOURCE) ../src/json.c ../src/sse.c \
    ../src/openai_client.c ../src/transport.c ../src/transport_socket.c \
    $(TEST_TLS_SOURCE) ../src/nevermore.c ../src/authinfo.c ../src/source.c \
    ../src/provider_hyper.c \
    ../src/provider_ollama.c ../src/provider_ollama_local.c \
    ../src/provider_openai.c \
    ../src/provider_openrouter.c \
    ../src/provider_opencode.c ../src/provider_opencode_zen.c \
    ../src/provider_test.c
test_chat_app_CFLAGS = $(TEST_CFLAGS) $(BOBA_CFLAGS) $(TEST_TLS_CFLAGS_EXTRA)
test_chat_app_LDFLAGS = $(TEST_TLS_LDFLAGS_EXTRA)
test_chat_app_LDADD = $(BOBA_LIBS) -lpthread $(TEST_TLS_LIBS)

# Each test binary gets a matching TESTS entry so `make check` runs it.
TESTS = $(check_PROGRAMS)

# Build the test binaries WITHOUT running them. CI uses this in the
# parallel Build step (make -j ... check-programs) so the timed Run
# tests step is pure execution — `make check` in the serial harness
# would otherwise compile all 15 binaries one-by-one inside it.
# (There is no built-in `make check_PROGRAMS`: the serial harness
# only expands that variable inside its own recursive rule.)
.PHONY: check-programs
check-programs: $(check_PROGRAMS)

# Verbose logging flag convention: pass -v to any test binary.
#
# Network gate: the suite must never make a REAL network call. Tests
# that exercise wire paths point at scripted loopback servers via
# explicit base URLs; but catalog sources default to probing the
# provider's default endpoint (local daemon 127.0.0.1:11434,
# api.openai.com, ...) when no base URL is given — on CI that probe's
# refusal latency is environment-dependent (a blackholed loopback
# connect stalled Windows CI past the 10s watchdog). NM_NO_LIVE_CATALOG
# makes default-base probes no-ops (static fallback); explicit-base
# fetches are unaffected.
#
# Config paths: every test that builds an app pins both config paths
# into its own scratch dir (nm_config_set_paths), and test_chat_app
# pins them to a nonexistent path in main() — a dev box's real
# ~/.config/nevermore/config would otherwise change captured wire
# requests, and a runtime write would land in the user's state dir.
# Nothing is exported here on purpose: the pin is per-test state, not
# an environment variable, so a test cannot forget to export it and a
# suite cannot silently inherit one.
TESTS_ENVIRONMENT = NM_NO_LIVE_CATALOG=1

EXTRA_DIST = test_helpers.h test_net_helpers.h fake_clock.h \
    tls/test-cert.pem tls/test-key.pem \
    tls/README.md

# The watchdog appends START/END pairs to test-progress.log by default
# (a hang is diagnosable from CI artifacts). It is a generated file;
# list it so `make distclean` removes it and distcheck's distcleancheck
# passes.
CLEANFILES = test-progress.log

# Wire tests (dummy local servers, no real network) — separate target,
# like quoth's `make test-wire`.
check-wire:
	@echo "wire tests not yet implemented (phase 1)"
