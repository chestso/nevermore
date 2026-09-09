# Standalone test binaries linked against the static library
check_PROGRAMS = \
    test_sse \
    test_json \
    test_provider

AM_CPPFLAGS = \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)/src \
    $(BOBA_CFLAGS)

LDADD = $(top_builddir)/src/libnevermore.a $(BOBA_LIBS)

# Each test binary gets a matching TESTS entry so `make check` runs it.
TESTS = $(check_PROGRAMS)

# Verbose logging flag convention: pass -v to any test binary.
TESTS_ENVIRONMENT =

EXTRA_DIST = test_helpers.h

# Wire tests (dummy servers, no network) — separate target, like
# quoth's `make test-wire`.
check-wire:
	@echo "wire tests not yet implemented (phase 1)"
