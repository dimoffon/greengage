# gpcontrib/gg_duckdb/Makefile
#
# DuckDB auxiliary executor for Greengage.  Built only when the tree was
# configured with --with-duckdb=PREFIX (see duckdb/build.sh for producing the
# prefix); gpcontrib/Makefile skips this directory otherwise.

MODULE_big = gg_duckdb
OBJS = src/gg_duckdb.o src/guc.o src/instance.o src/funcs.o src/types.o src/leaf.o src/node.o src/pass.o

EXTENSION = gg_duckdb
DATA = gg_duckdb--1.0.sql

REGRESS = setup basic region teardown

with_llvm = no

# Where DuckDB is.  In-tree builds take it from configure; PGXS builds from
# DUCKDB_PREFIX, since pg_config does not expose --with-duckdb.
ifdef USE_PGXS
with_duckdb ?= $(DUCKDB_PREFIX)
else
subdir = gpcontrib/gg_duckdb
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
endif

ifeq ($(with_duckdb),)
$(error gg_duckdb needs a DuckDB prefix: configure --with-duckdb=PREFIX, or make DUCKDB_PREFIX=PREFIX USE_PGXS=1)
endif

# pgxs.mk reads PG_CPPFLAGS when it is included, so the flags come first.
# DuckDB headers are -isystem: the tree compiles with -Werror and the header's
# warnings are not ours.  The module links against libduckdb.so from the prefix
# at build time and finds the copy installed next to the Greengage libraries at
# run time ($ORIGIN/.. from lib/postgresql/).
PG_CPPFLAGS = -I$(srcdir)/include -isystem $(with_duckdb)/include \
	-DGG_DUCKDB_PINNED_VERSION='"$(shell sed -n 's/^DUCKDB_VERSION=//p' $(srcdir)/duckdb/duckdb.version)"'
SHLIB_LINK = -L$(with_duckdb)/lib -lduckdb -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$(libdir)'

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
include $(top_srcdir)/contrib/contrib-global.mk
endif

REGRESS_OPTS += --init-file=$(top_srcdir)/src/test/regress/init_file --init-file=$(srcdir)/init_file

install: install-duckdb-lib

.PHONY: install-duckdb-lib
install-duckdb-lib:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'
	$(INSTALL_SHLIB) '$(with_duckdb)/lib/libduckdb.so' '$(DESTDIR)$(libdir)/libduckdb.so'
