# tests/Makefile.am - standalone test binaries (link sources directly,
# portty style — no library to link against)

check_PROGRAMS = \
    test_sse \
    test_json \
    test_provider

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

# Provider registry (nevermore.c + the four provider stubs; no TLS
# or sockets involved).
test_provider_SOURCES = test_provider.c ../src/nevermore.c \
    ../src/provider_hyper.c ../src/provider_ollama.c \
    ../src/provider_openai.c ../src/provider_openrouter.c
test_provider_CFLAGS = $(TEST_CFLAGS)

# Each test binary gets a matching TESTS entry so `make check` runs it.
TESTS = $(check_PROGRAMS)

# Verbose logging flag convention: pass -v to any test binary.
TESTS_ENVIRONMENT =

EXTRA_DIST = test_helpers.h

# Wire tests (dummy local servers, no real network) — separate target,
# like quoth's `make test-wire`.
check-wire:
	@echo "wire tests not yet implemented (phase 1)"
