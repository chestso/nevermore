# tests/Makefile.am - standalone test binaries (link sources directly,
# portty style — no library to link against)

check_PROGRAMS = \
    test_sse \
    test_json \
    test_provider \
    test_wire \
    test_openai_client \
    test_tls

AM_CPPFLAGS = \
    -I$(top_srcdir)/src \
    -I$(top_builddir) \
    $(BOBA_CFLAGS)

TEST_CFLAGS = -Wall -Wextra

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
    ../src/transport.c ../src/transport_socket.c ../src/tls_openssl.c
test_provider_CFLAGS = $(TEST_CFLAGS) $(OPENSSL_CFLAGS)
test_provider_LDADD = $(OPENSSL_LIBS)

# Transport wire tests (dummy threaded localhost servers, canned
# responses; loopback only, never real APIs). POSIX threads.
test_wire_SOURCES = test_wire.c ../src/transport.c     ../src/transport_socket.c ../src/tls_openssl.c
test_wire_CFLAGS = $(TEST_CFLAGS) $(OPENSSL_CFLAGS)
test_wire_LDADD = -lpthread $(OPENSSL_LIBS)

# End-to-end: the shared OpenAI-compatible client against a canned
# chat/completions SSE server (request validation + delta assembly).
test_openai_client_SOURCES = test_openai_client.c \
    ../src/openai_client.c ../src/json.c ../src/sse.c \
    ../src/transport.c ../src/transport_socket.c ../src/tls_openssl.c \
    ../src/nevermore.c ../src/provider_hyper.c ../src/provider_ollama.c \
    ../src/provider_openai.c ../src/provider_openrouter.c
test_openai_client_CFLAGS = $(TEST_CFLAGS) -I$(top_builddir)/src $(OPENSSL_CFLAGS)
test_openai_client_LDADD = -lpthread $(OPENSSL_LIBS)

# TLS: negative handshake over loopback with the committed self-signed
# cert (system trust must reject it). Live probe is manual
# (NM_LIVE_TLS=1), never in make check.
test_tls_SOURCES = test_tls.c ../src/transport.c ../src/transport_socket.c \
    ../src/tls_openssl.c
test_tls_CFLAGS = $(TEST_CFLAGS) $(OPENSSL_CFLAGS)
test_tls_LDADD = -lpthread $(OPENSSL_LIBS)

# Each test binary gets a matching TESTS entry so `make check` runs it.
TESTS = $(check_PROGRAMS)

# Verbose logging flag convention: pass -v to any test binary.
TESTS_ENVIRONMENT =

EXTRA_DIST = test_helpers.h tls/test-cert.pem tls/test-key.pem tls/README.md

# Wire tests (dummy local servers, no real network) — separate target,
# like quoth's `make test-wire`.
check-wire:
	@echo "wire tests not yet implemented (phase 1)"
