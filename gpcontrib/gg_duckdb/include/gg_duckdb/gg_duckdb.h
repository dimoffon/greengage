/*-------------------------------------------------------------------------
 *
 * gg_duckdb.h
 *	  DuckDB auxiliary executor for Greengage: shared declarations.
 *
 * Everything here runs on the backend's main thread.  The embedded DuckDB is
 * opened with threads=1 and external_threads=1, so DuckDB never owns a thread
 * of its own and every DuckDB callback (table functions included) runs on
 * the thread that drives duckdb_pending_execute_task().  That is what makes
 * it legal for those callbacks to call into the backend.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GG_DUCKDB_H
#define GG_DUCKDB_H

#include "postgres.h"

#include <duckdb.h>

#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/plannodes.h"

/* gg_duckdb.mode */
typedef enum GGDuckMode
{
	GG_DUCKDB_MODE_OFF,				/* never plan or execute DuckDB regions */
	GG_DUCKDB_MODE_AUTO,			/* cost-gated */
	GG_DUCKDB_MODE_FORCE			/* every eligible region, no cost gate */
} GGDuckMode;

/* gg_duckdb.debug_wrap */
typedef enum GGDuckDebugWrap
{
	GG_DUCKDB_WRAP_OFF,
	GG_DUCKDB_WRAP_SCANS			/* every SeqScan becomes an identity region */
} GGDuckDebugWrap;

/* GUCs (guc.c) */
extern int	gg_duckdb_mode;
extern int	gg_duckdb_max_memory_mb;
extern char *gg_duckdb_temp_directory;
extern char *gg_duckdb_max_temp_directory_size;
extern bool gg_duckdb_release_instance_at_end;
extern int	gg_duckdb_debug_wrap;
extern char *gg_duckdb_debug_region_sql;

extern void gg_duckdb_define_gucs(void);

/* Whether _PG_init ran from shared_preload_libraries (gg_duckdb.c). */
extern bool gg_duckdb_preloaded;

/* Counters reported by gg_duckdb.status() (instance.c). */
typedef struct GGDuckStats
{
	int64		queries_executed;
	int64		errors;
	char	   *last_error;		/* TopMemoryContext copy, or NULL */
} GGDuckStats;

extern GGDuckStats gg_duckdb_stats;

/* Instance and connections (instance.c) */
extern bool gg_duckdb_instance_initialized(void);
extern duckdb_database gg_duckdb_instance(void);
extern duckdb_connection gg_duckdb_connect(void);
extern void gg_duckdb_disconnect(duckdb_connection *conn);
extern void gg_duckdb_instance_close(void);
extern const char *gg_duckdb_instance_temp_directory(void);
extern int64 gg_duckdb_instance_memory_limit_bytes(void);
extern int	gg_duckdb_instance_threads(void);
extern void gg_duckdb_note_error(const char *msg);

/*
 * Drive a pending DuckDB result to completion on this thread, honouring
 * query cancel.  Returns with the pending result ready to be executed, or
 * raises a PG error after releasing nothing: callers own the DuckDB objects
 * and must free them in their PG_CATCH.
 */
extern void gg_duckdb_run_pending(duckdb_connection conn,
								  duckdb_pending_result pending);

/* ---------- types (types.c) ---------- */

typedef struct GGTypeInfo
{
	Oid			typid;
	int32		typmod;
	duckdb_type duck;			/* DuckDB logical type id */
	uint8		width;			/* DECIMAL precision */
	uint8		scale;			/* DECIMAL scale */
} GGTypeInfo;

extern bool gg_duckdb_type_map(Oid typid, int32 typmod, GGTypeInfo *ti);
extern duckdb_logical_type gg_duckdb_logical_type(const GGTypeInfo *ti);
extern const char *gg_duckdb_type_name(duckdb_type t);
extern void gg_duckdb_write_datum(const GGTypeInfo *ti, duckdb_vector vec, void *data,
								  uint64_t *validity, idx_t row, Datum d, bool isnull);
extern Datum gg_duckdb_read_datum(const GGTypeInfo *ti, duckdb_type vt, int width, int scale,
								  void *data, uint64_t *validity, idx_t row, bool *isnull);

/* ---------- the region node (node.c) and its leaves (leaf.c) ---------- */

#define GG_DUCKDB_REGION_NAME		"GGDuckDBRegion"
#define GG_DUCKDB_LEAF_FUNCTION		"gg_leaf"
#define GG_DUCKDB_PRIVATE_VERSION	1

/* positions in CustomScan.custom_private, a flat List of Const */
enum
{
	GGP_VERSION = 0,			/* int4 */
	GGP_SQL,					/* text: the DuckDB query */
	GGP_FLAGS,					/* int4 */
	GGP_NLEAVES,				/* int4 */
	GGP_LABEL,					/* text: what EXPLAIN shows */
	GGP_NPARAMS,				/* int4, followed by that many Consts */
	GGP_NFIELDS
};

typedef struct GGLeafCol
{
	char		name[16];		/* "c<k>" as DuckDB sees it */
	GGTypeInfo	type;
} GGLeafCol;

typedef struct GGLeaf
{
	PlanState  *ps;
	int			ncols;
	GGLeafCol  *cols;			/* one per result column of the leaf */
	double		plan_rows;
	bool		eof;
	int64		rows;			/* pulled so far */
} GGLeaf;

typedef struct GGRegionState
{
	CustomScanState css;		/* custom_ps holds the leaf PlanStates */

	/* from custom_private */
	int			version;
	const char *sql;
	int			flags;
	int			nleaves;
	const char *label;

	GGLeaf	   *leaves;

	/* output columns, from the scan tuple descriptor */
	int			ncols;
	GGTypeInfo *outtypes;
	void	  **coldata;		/* per column of the current chunk */
	uint64_t  **colvalid;
	int		   *colwidth;		/* DECIMAL width/scale seen in the chunk */
	int		   *colscale;

	MemoryContext region_cxt;	/* lives with the node */
	MemoryContext chunk_cxt;	/* reset before every fetched chunk */
	MemoryContext batch_cxt;	/* reset before every leaf batch */

	/* DuckDB objects, released idempotently */
	duckdb_connection conn;
	duckdb_prepared_statement stmt;
	duckdb_pending_result pending;
	duckdb_result result;
	bool		has_result;
	duckdb_data_chunk chunk;
	idx_t		chunk_size;
	idx_t		chunk_row;

	bool		started;
	bool		finished;
	ErrorData  *pending_error;	/* a PG error raised inside a leaf callback */

	int64		rows_out;
	int64		chunks;
} GGRegionState;

extern CustomScanMethods gg_duckdb_scan_methods;

extern List *gg_duckdb_make_private(const char *sql, int flags, int nleaves,
									const char *label);
extern void gg_duckdb_register_leaf_function(duckdb_connection conn);
extern duckdb_prepared_statement gg_duckdb_prepare_region(GGRegionState *region,
														  duckdb_connection conn,
														  const char *sql,
														  char **errmsg);

/* ---------- the planner pass (pass.c) ---------- */

extern void gg_duckdb_install_planner_hook(void);

#endif							/* GG_DUCKDB_H */
