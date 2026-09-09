# isolation2 tests of gg_duckdb: cancellation, timeouts, early stop across
# slices, memory limits, errors inside leaves and nested regions, driven by
# fault injection (needs a build with --enable-debug-extensions).
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

EXTRA_REGRESS_OPTS += --init-file=$(top_builddir)/src/test/regress/init_file --init-file=$(srcdir)/init_file

installcheck:
	$(pg_isolation2_regress_installcheck) \
	--dbname=ggduckdb_isolation2 --schedule=isolation2_schedule
