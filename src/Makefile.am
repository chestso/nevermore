# Build static library
lib_LIBRARIES = libnevermore.a

# nevermore_version.h is regenerated on every `make` so the embedded version
# string tracks the current git HEAD (including a -dirty suffix when the
# working tree has uncommitted changes). The recipe always runs, but the
# file is only rewritten when the version actually changes — so dependent
# .o files stay cached when nothing changed.
BUILT_SOURCES = nevermore_version.h
CLEANFILES = nevermore_version.h
nodist_libnevermore_a_SOURCES = nevermore_version.h

.PHONY: nevermore_version_force
nevermore_version.h: nevermore_version_force
	@v=`"$(top_srcdir)/build-aux/git-version.sh" "$(top_srcdir)" 2>/dev/null`; \
        test -n "$$v" || v="$(NEVERMORE_VERSION)"; \
        test -n "$$v" || v="unknown"; \
        base=`echo "$$v" | sed 's/-.*//'`; \
        major=`echo "$$base" | cut -d. -f1`; \
        minor=`echo "$$base" | cut -d. -f2`; \
        patch=`echo "$$base" | cut -d. -f3`; \
        printf '#ifndef NEVERMORE_VERSION_H\n#define NEVERMORE_VERSION_H\n#define NEVERMORE_VERSION "%s"\n#define NEVERMORE_VERSION_MAJOR %s\n#define NEVERMORE_VERSION_MINOR %s\n#define NEVERMORE_VERSION_PATCH %s\n#endif\n' "$$v" "$$major" "$$minor" "$$patch" > $@.tmp; \
        if cmp -s $@.tmp $@ 2>/dev/null; then \
          rm -f $@.tmp; \
        else \
          mv $@.tmp $@; \
          echo "  GEN      $@ ($$v)"; \
        fi
nevermore_version_force:

AM_CPPFLAGS = \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)/src \
    $(BOBA_CFLAGS)

# Core sources. TLS backends are conditionally included below; each
# backend .c guards itself with its NM_TLS_* define so at most one
# provides real symbols (the others compile to translation units with
# no exported symbols).
CORE_SOURCES = \
    nevermore.c \
    connection_layout.h \
    provider_internal.h \
    provider_hyper.c \
    provider_ollama.c \
    provider_openai.c \
    provider_openrouter.c \
    openai_client.h \
    openai_client.c \
    transport.c \
    transport_socket.c \
    sse.c \
    json.c \
    agent.c \
    tools.c \
    tools_file.c \
    session.c

libnevermore_a_SOURCES = $(CORE_SOURCES)

# TLS: exactly one backend, selected by configure.
if TLS_SCHANNEL
libnevermore_a_SOURCES += tls_schannel.c
endif
if TLS_SECTRANSPORT
libnevermore_a_SOURCES += tls_sectransport.c
endif
if TLS_MBEDTLS
libnevermore_a_SOURCES += tls_mbedtls.c
endif
if TLS_OPENSSL
libnevermore_a_SOURCES += tls_openssl.c
endif

# Portable process spawn: one file per OS.
if HOST_WINDOWS
libnevermore_a_SOURCES += tools_spawn_win.c os_compat_win.c
else
libnevermore_a_SOURCES += tools_spawn_posix.c
endif
