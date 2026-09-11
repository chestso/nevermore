# src/Makefile.am - Build rules for nevermore source files

# Regenerate nevermore_version.h on every `make` so the embedded version
# string tracks the current git HEAD (including a -dirty suffix when the
# working tree has uncommitted changes). The recipe always runs, but the
# file is only rewritten when the version actually changes — so dependent
# .o files stay cached when nothing changed.
BUILT_SOURCES = nevermore_version.h
CLEANFILES = nevermore_version.h

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

bin_PROGRAMS = nevermore

# Internal headers (no separate public include/ — CLI app, no library).
EXTRA_DIST = \
    connection_layout.h \
    provider_internal.h \
    tools_internal.h \
    transport_internal.h \
    openai_client.h

# Portable process spawn: one file per OS.
if HOST_WINDOWS
TOOLS_SPAWN_SOURCE = tools_spawn_win.c os_compat_win.c
else
TOOLS_SPAWN_SOURCE = tools_spawn_posix.c
endif

# TLS: exactly one backend, selected by configure. Each backend .c
# guards itself with its NM_TLS_* define so at most one provides real
# symbols (the others compile to translation units with no exported
# symbols).
if TLS_SCHANNEL
TLS_SOURCE = tls_schannel.c
endif
if TLS_SECTRANSPORT
TLS_SOURCE = tls_sectransport.c
endif
if TLS_MBEDTLS
TLS_SOURCE = tls_mbedtls.c
endif
if TLS_OPENSSL
TLS_SOURCE = tls_openssl.c
endif

# All sources in one program — this is a CLI/TUI app, not a library.
nevermore_SOURCES = \
    nevermore_version.h \
    main.c \
    nevermore.c \
    provider_internal.h \
    provider_hyper.c \
    provider_ollama.c \
    provider_openai.c \
    provider_openrouter.c \
    openai_client.h \
    openai_client.c \
    transport_internal.h \
    transport.c \
    connection_layout.h \
    transport_socket.c \
    sse.c \
    json.c \
    agent.c \
    source.h \
    source.c \
    tools_internal.h \
    tools.c \
    tools_file.c \
    session.c \
    chat_app.h \
    chat_app.c \
    history.h \
    history.c \
    spinner.h \
    spinner.c \
    colors.h \
    $(TOOLS_SPAWN_SOURCE) \
    $(TLS_SOURCE)

nevermore_CPPFLAGS = \
    -I$(top_builddir) \
    -I$(top_srcdir)/src \
    $(BOBA_CFLAGS) \
    $(OPENSSL_CFLAGS) \
    $(MBEDTLS_CFLAGS)

nevermore_LDADD = $(BOBA_LIBS) $(OPENSSL_LIBS) $(MBEDTLS_LIBS)

# Windows resource file — embed application icon into the .exe via windres.
if HAVE_WINDRES
noinst_DATA = nevermore-res.o
nevermore_LDADD += nevermore-res.o

nevermore-res.o: $(srcdir)/nevermore.rc $(top_srcdir)/icons/nevermore.ico
	$(WINDRES) \
	    --define _UNICODE --define UNICODE \
	    -I$(top_srcdir) \
	    -I$(srcdir) \
	    $(srcdir)/nevermore.rc -O coff -o $@
endif
