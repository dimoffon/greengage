/*-------------------------------------------------------------------------
 *
 * gg_duckdb.c
 *	  DuckDB auxiliary executor for Greengage: module entry point.
 *
 * The library is meant to sit in shared_preload_libraries on the coordinator
 * and on every segment: a plan node produced by it is resolved by name on the
 * QE that receives the plan.  Loading it with LOAD or CREATE EXTENSION alone
 * still works for the SQL functions, but the planner side stays off and
 * gg_duckdb.status() reports preloaded = false.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "storage/ipc.h"

#include "gg_duckdb/gg_duckdb.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

bool		gg_duckdb_preloaded = false;

static void
gg_duckdb_proc_exit(int code, Datum arg)
{
	gg_duckdb_instance_close();
}

void
_PG_init(void)
{
	gg_duckdb_preloaded = process_shared_preload_libraries_in_progress;

	gg_duckdb_define_gucs();

	/* the region node is looked up by name on the QE that receives a plan */
	RegisterCustomScanMethods(&gg_duckdb_scan_methods);
	gg_duckdb_install_planner_hook();

	/*
	 * Registered in whichever process loads us.  Under shared_preload_libraries
	 * that is the postmaster, whose children inherit the callback; the DuckDB
	 * instance itself is only ever created lazily in a backend.
	 */
	on_proc_exit(gg_duckdb_proc_exit, (Datum) 0);
}
