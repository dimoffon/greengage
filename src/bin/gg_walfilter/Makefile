# src/bin/gg_walfilter/Makefile

PGFILEDESC = "gg_walfilter - record-level filtering of archived WAL"
PGAPPICON = win32

subdir = src/bin/gg_walfilter
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = gg_walfilter.o segment.o filter.o state.o fetch.o $(WIN32RES)

override CPPFLAGS := -DFRONTEND $(CPPFLAGS)

all: gg_walfilter

gg_walfilter: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) gg_walfilter$(X) '$(DESTDIR)$(bindir)/gg_walfilter$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/gg_walfilter$(X)'

clean distclean maintainer-clean:
	rm -f gg_walfilter$(X) $(OBJS)
