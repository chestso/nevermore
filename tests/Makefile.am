# tests/Makefile.am - standalone test binaries (link sources directly,
# portty style — no library to link against)

check_PROGRAMS = \
    test_sse \
    test_json \
    test_provider \
    test_wire \
    test_openai_client \
    test_tls \
    test_session \
    test_tools \
    test_agent

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_builddir) \
    $(BOBA_CFLAGS)

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
test_provider_LDADD = $(TEST_TLS_LIBS)

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

# Each test binary gets a matching TESTS entry so `make check` runs it.
TESTS = $(check_PROGRAMS)

# Verbose logging flag convention: pass -v to any test binary.
TESTS_ENVIRONMENT =

EXTRA_DIST = test_helpers.h tls/test-cert.pem tls/test-key.pem tls/README.md

# Wire tests (dummy local servers, no real network) — separate target,
# like quoth's `make test-wire`.
check-wire:
	@echo "wire tests not yet implemented (phase 1)"
