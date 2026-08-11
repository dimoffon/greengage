# src/bin/gg_topology/Makefile

PGFILEDESC = "gg_topology - inspect and replace the cluster topology store"
PGAPPICON = win32

subdir = src/bin/gg_topology
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = gg_topology.o $(WIN32RES)

override CPPFLAGS := -DFRONTEND $(CPPFLAGS)

all: gg_topology

gg_topology: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) gg_topology$(X) '$(DESTDIR)$(bindir)/gg_topology$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/gg_topology$(X)'

clean distclean maintainer-clean:
	rm -f gg_topology$(X) $(OBJS)
