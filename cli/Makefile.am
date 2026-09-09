# Build CLI executable
bin_PROGRAMS = nevermore

nevermore_SOURCES = \
    main.c \
    chat_app.c \
    history.c \
    spinner.c

nevermore_CPPFLAGS = \
    -I$(top_builddir) \
    -I$(top_builddir)/src \
    -I$(top_srcdir)/include \
    -I$(top_srcdir)/src \
    -I$(srcdir) \
    $(BOBA_CFLAGS)

# nevermore_version.h is generated in $(top_builddir)/src. Force it to exist
# before any CLI objects compile.
$(nevermore_OBJECTS): $(top_builddir)/src/nevermore_version.h
$(top_builddir)/src/nevermore_version.h:
	$(MAKE) -C $(top_builddir)/src nevermore_version.h

nevermore_LDADD = \
    $(top_builddir)/src/libnevermore.a \
    $(BOBA_LIBS)

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

EXTRA_DIST = chat_app.h history.h spinner.h colors.h
