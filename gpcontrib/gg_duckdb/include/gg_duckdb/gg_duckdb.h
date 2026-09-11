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
#include "utils/rel.h"

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
extern int	gg_duckdb_min_memory_mb;
extern char *gg_duckdb_temp_directory;
extern char *gg_duckdb_max_temp_directory_size;
extern bool gg_duckdb_release_instance_at_end;
extern int	gg_duckdb_debug_wrap;
extern char *gg_duckdb_debug_region_sql;
extern int	gg_duckdb_min_rows;
extern bool gg_duckdb_explain_decisions;
extern bool gg_duckdb_validate_at_plan_time;
extern bool gg_duckdb_on_coordinator;
extern bool gg_duckdb_reserve_memory;
extern bool gg_duckdb_strict;
extern char *gg_duckdb_data_directories;
extern char *gg_duckdb_http_proxy;
extern bool gg_duckdb_allow_float_aggregates;
extern double gg_duckdb_cost_fixed;
extern double gg_duckdb_cost_convert_factor;
extern double gg_duckdb_cost_op_factor;
extern double gg_duckdb_cost_margin;
extern bool gg_duckdb_cost_boundary;

extern void gg_duckdb_define_gucs(void);

/* Plan hints (hints.c) */
typedef struct GGHints GGHints;
typedef enum GGHintVerdict
{
	GG_HINT_NONE,
	GG_HINT_FORCE,
	GG_HINT_FORBID
} GGHintVerdict;

extern GGHints *gg_duckdb_hints_collect(Query *parse, PlannedStmt *stmt);
extern List *gg_duckdb_region_aliases(PlannedStmt *stmt, List *leaves);
extern GGHintVerdict gg_duckdb_hints_verdict(GGHints *hints, List *aliases);
extern void gg_duckdb_hints_report(GGHints *hints);

/* Whether _PG_init ran from shared_preload_libraries (gg_duckdb.c). */
extern bool gg_duckdb_preloaded;

/* Counters reported by gg_duckdb.status() (instance.c). */
typedef struct GGDuckStats
{
	int64		queries_executed;
	int64		errors;
	int64		reopens;		/* instances reopened after invalidation */
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
/*
 * Where a column's values go: the vector's data and validity, and for a
 * numeric aggregate state the two children.  Resolved before any PostgreSQL
 * work starts, so that filling a row is plain memory writes (see leaf.c on
 * why no DuckDB call may sit inside a PG_TRY block).
 */
typedef struct GGVectorTarget
{
	duckdb_vector vec;
	void	   *data;
	uint64_t   *validity;
	void	   *sum_data;		/* STRUCT: the sum child */
	uint64_t   *sum_validity;
	void	   *count_data;		/* STRUCT: the count child */
} GGVectorTarget;

extern void gg_duckdb_vector_target(duckdb_vector vec, const GGTypeInfo *ti,
									GGVectorTarget *t);
extern void gg_duckdb_write_datum(const GGTypeInfo *ti, const GGVectorTarget *t,
								  idx_t row, Datum d, bool isnull,
								  const char **str, int *len);
extern Datum gg_duckdb_read_datum(const GGTypeInfo *ti, duckdb_type vt, int width, int scale,
								  duckdb_vector vec, void *data, uint64_t *validity, idx_t row,
								  bool *isnull);

/* ---------- the region node (node.c) and its leaves (leaf.c) ---------- */

#define GG_DUCKDB_REGION_NAME		"GGDuckDBRegion"
#define GG_DUCKDB_LEAF_FUNCTION		"gg_leaf"
#define GG_DUCKDB_LEAF_PLACEHOLDER "_gg_row"
#define GG_DUCKDB_PRIVATE_VERSION	4

/*
 * Positions in CustomScan.custom_private, a flat List of Const (the only
 * node kinds every plan walker accepts there):
 *   version, sql, flags, nleaves, label, nparams, <nparams Consts bound as
 *   $1..$n>, nout, <nout int4 output descriptors: duck type << 16 | width
 *   << 8 | scale>.
 */
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

#define GG_OUTDESC(ti)	(((int) (ti)->duck << 16) | ((int) (ti)->width << 8) | (int) (ti)->scale)

typedef struct GGLeafCol
{
	char		name[16];		/* "c<k>" as DuckDB sees it */
	GGTypeInfo	type;
} GGLeafCol;

/* what gg_leaf's bind needs: shared by the QE (from PlanStates) and the QD (from Plans) */
typedef struct GGLeafDesc
{
	int			ncols;
	GGLeafCol  *cols;			/* one per result column of the leaf */
	double		plan_rows;
} GGLeafDesc;

typedef struct GGLeaf
{
	GGLeafDesc	desc;
	PlanState  *ps;
	bool		eof;
	int64		rows;			/* pulled so far */
} GGLeaf;

struct GGRegionState;

/* what is visible to gg_leaf while a region query is prepared */
typedef struct GGBindContext
{
	struct GGRegionState *region;	/* NULL when only validating on the QD */
	int			nleaves;
	GGLeafDesc **leaves;
} GGBindContext;

/*
 * A DuckDB query driven by a plan node on the backend thread: the region
 * (node.c) and the native foreign scan (fdw.c) share it.  Rows of the
 * streamed result are converted into the node's scan slot; `colmap` says
 * which slot attribute a result column fills (NULL: column i fills
 * attribute i, the rest of the slot is NULL).
 */
typedef struct GGDuckQuery
{
	PlanState  *ps;				/* the node running the query: memory quota */
	const char *what;			/* "region" or "foreign scan", for messages */
	const char *sql;
	List	   *params;			/* Consts and executor Params bound as $1..$n */
	List	   *file_lists;		/* List of List of char *, bound as $n+1.. */
	List	   *pre_sql;		/* statements run on the connection before the query */

	int			ncols;			/* result columns */
	GGTypeInfo *outtypes;
	int		   *colmap;			/* result column -> 0-based slot attribute, or NULL */
	int64		memory_reserved;	/* bytes reserved with the vmem tracker */
	duckdb_vector *colvec;
	void	  **coldata;		/* per column of the current chunk */
	uint64_t  **colvalid;
	int		   *colwidth;		/* DECIMAL width/scale seen in the chunk */
	int		   *colscale;

	MemoryContext query_cxt;	/* lives with the node */
	MemoryContext chunk_cxt;	/* reset before every fetched chunk */
	MemoryContext batch_cxt;	/* reset before every leaf batch */

	/* DuckDB objects, released idempotently */
	duckdb_connection conn;
	duckdb_prepared_statement stmt;
	const char *prepared_sql;	/* what stmt was prepared from (query_cxt) */
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
} GGDuckQuery;

/* query.c */
extern void gg_duckdb_query_init(GGDuckQuery *q, PlanState *ps, MemoryContext parent,
								 const char *what);
extern void gg_duckdb_query_set_output(GGDuckQuery *q, TupleDesc desc,
									   GGTypeInfo *outtypes, int ncols, int *colmap);
extern void gg_duckdb_query_start(GGDuckQuery *q, GGBindContext *bctx);
extern bool gg_duckdb_query_next(GGDuckQuery *q, TupleTableSlot *slot);
extern void gg_duckdb_query_reset(GGDuckQuery *q);
extern void gg_duckdb_query_release_query(GGDuckQuery *q);
extern void gg_duckdb_query_release(GGDuckQuery *q);
extern void gg_duckdb_query_raise(GGDuckQuery *q, const char *what, const char *msg) pg_attribute_noreturn();
extern void gg_duckdb_query_explain_end(GGDuckQuery *q, StringInfo buf);

/*
 * A native leaf: a gg_duckdb foreign scan the region reads through DuckDB
 * itself.  The region's SQL carries the token GG_NATIVE_TOKEN(i) where the
 * reader call goes; the executing node replaces it with the reader over
 * its share of the files, or with an empty relation when it has none.
 */
#define GG_NATIVE_TOKEN_FMT "__GG_NATIVE_%d__"

typedef struct GGNativeLeaf
{
	Oid			relid;			/* the foreign table */
	char	   *reader;			/* "read_parquet(%s, ...)" with %s for the file list parameter */
	char	   *empty;			/* the empty relation of the same columns */
} GGNativeLeaf;

typedef struct GGRegionState
{
	CustomScanState css;		/* custom_ps holds the leaf PlanStates */
	GGDuckQuery q;

	/* from custom_private */
	int			version;
	int			flags;
	int			nleaves;
	const char *label;
	const char *sql;			/* with native tokens; q.sql is the executable text */
	List	   *natives;		/* of GGNativeLeaf */

	GGLeaf	   *leaves;
	GGTypeInfo **leaf_shapes;	/* DuckDB shape of every leaf column, from the plan */
	int		   *leaf_ncols;
} GGRegionState;

extern CustomScanMethods gg_duckdb_scan_methods;

extern char *gg_duckdb_region_sql(const char *sql, List *natives, int nconst_params,
								  bool resolve, List **file_lists);
extern List *gg_duckdb_make_private(const char *sql, int flags, List *leaves,
									const char *label, List *params, int nout,
									const GGTypeInfo *outtypes, List *natives);
extern void gg_duckdb_register_leaf_function(duckdb_connection conn);
extern duckdb_prepared_statement gg_duckdb_prepare_with(GGBindContext *bctx,
														duckdb_connection conn,
														const char *sql,
														char **errmsg);
extern void gg_duckdb_describe_leaf_plan(GGLeafDesc *desc, Plan *plan);
extern GGBindContext *gg_duckdb_bind_context_push(GGBindContext *bctx);
extern void gg_duckdb_bind_context_pop(GGBindContext *saved);

/* ---------- the deparser (deparse.c) ---------- */

typedef struct GGRegionSpec
{
	char	   *sql;
	List	   *leaves;			/* Plan nodes, in gg_leaf index order */
	int			nleaves;
	List	   *params;			/* Consts and executor Params bound as $1..$n */
	int			nexecparams;	/* of those, executor parameters */
	int			ncols;
	GGTypeInfo *outtypes;		/* DuckDB shape of every output column */
	int			flags;
	char	   *label;
	const char *reject;			/* why the subtree is ineligible, or NULL */
	double		rows_in;		/* estimated rows entering DuckDB */
	double		bytes_in;		/* estimated bytes entering DuckDB */
	double		conv_in;		/* planner-unit cost of converting the leaves' used columns */
	double		op_cost;		/* PostgreSQL-unit cost of the interior operators */
	double		rows_out;		/* estimated rows the region returns */
	double		bytes_out;
	double		conv_out;		/* planner-unit cost of converting the output */
	double		parent_limit;	/* in: rows a Limit right above the root keeps, else 0 */
	bool		rescanned;		/* the root has outer parameters: expect rescans */
	int			ninterior;
	int			naggs;
	int			nsorts;
	int			njoins;
	bool		ordered;		/* the query ends with the root's ORDER BY */
	char	   *agg_order;		/* ORDER BY restoring a sorted Agg's output order, or NULL */
	List	   *natives;		/* of GGNativeLeaf: foreign tables DuckDB reads itself */
	double		rows_native;	/* of rows_in, the rows DuckDB reads natively */
	List	   *rtable;			/* in: the statement's range table */
	bool		cost_boundary;	/* in: the cost model may end the region above a subtree */
	int			ncuts;			/* out: subtrees the executor keeps as leaves */
	List	   *cuts;			/* out: of GGRegionCut, for the decision log */
} GGRegionSpec;

/* A subtree the cost model left with the executor, and the numbers that decided it. */
typedef struct GGRegionCut
{
	Plan	   *plan;
	double		rows_out;		/* rows the executor hands the region */
	double		leaf_cost;		/* gate units: converting that output */
	double		rows_in;		/* rows the region would have converted instead */
	double		interior_cost;	/* gate units: converting those, less DuckDB's gain on the operators */
} GGRegionCut;

/* A gg_duckdb foreign table as DuckDB reads it (fdw.c describes, deparse.c writes). */
typedef struct GGNativeInfo
{
	int			natts;
	char	  **colnames;		/* by attribute number - 1: quoted file column names */
	GGTypeInfo *types;			/* by attribute number - 1; duck == 0 when not fetched */
	char	   *reader;			/* "read_parquet(%s, ...)", %s for the file list parameter */
	char	   *empty;			/* the empty relation with the fetched columns */
} GGNativeInfo;

typedef struct GGNativeScan
{
	Oid			relid;			/* in */
	Index		scanrelid;		/* in: varno of the quals' Vars */
	List	   *attnos;			/* in: fetched attributes, ascending */
	List	   *quals;			/* in: quals DuckDB evaluates */
	char	   *sql;			/* out: the scan query with the reader token */
	char	   *reader;
	char	   *empty;
	GGTypeInfo *types;
	int			natts;
	const char *reject;
} GGNativeScan;

struct DeparseCtx;
extern bool gg_duckdb_deparse_native_scan(struct DeparseCtx *ctx, GGNativeScan *ns, const char *token);
extern bool gg_duckdb_native_scan_sql(GGNativeScan *ns, const char *token, List **params);
extern bool gg_duckdb_native_qual_ok(Oid relid, Index scanrelid, List *attnos, Node *qual);

/* fdw.c */
extern List *gg_duckdb_native_files(Oid relid);
extern bool gg_duckdb_native_describe(Oid relid, List *attnos, GGNativeInfo *info, const char **reject);
extern bool gg_duckdb_is_native_scan(ForeignScan *fs, List *rtable);
extern bool gg_duckdb_foreign_scan_private(ForeignScan *fs, List **attnos, List **quals);
extern char *gg_duckdb_native_location_text(Oid relid);
extern List *gg_duckdb_native_pre_sql(List *natives);

extern bool gg_duckdb_deparse_region(PlannedStmt *stmt, Plan *root, GGRegionSpec *spec);
extern const char *gg_duckdb_type_sql(const GGTypeInfo *ti);

/* fdw.c: the options of a foreign table, table over server over wrapper */
typedef struct GGForeignOptions
{
	char	   *format;			/* parquet, csv, json or iceberg */
	List	   *locations;		/* of char *: paths or globs */
	bool		hive_partitioning;
	bool		union_by_name;
	char	   *json_format;	/* auto, newline_delimited, array, unstructured; NULL: by location */
	List	   *reader_opts;	/* DefElems passed to the reader: header, delim, ... */
	Oid			serverid;
	bool		catalog;		/* an Iceberg table of the server's REST catalog */
	char	   *catalog_ref;	/* its DuckDB name: gg_ice_<server>."ns"."table" */
} GGForeignOptions;

extern void gg_duckdb_foreign_options(Oid relid, GGForeignOptions *o);
extern void gg_duckdb_iceberg_attach(duckdb_connection conn, Oid serverid);
extern char *gg_duckdb_iceberg_attach_sql(Oid serverid);
extern void gg_duckdb_fdw_instance_closed(void);
extern void gg_duckdb_check_locations(List *locations);
extern void gg_duckdb_ensure_s3_secrets(Oid relid, GGForeignOptions *o);
extern char *gg_duckdb_duck_literal(const char *s);
extern char *gg_duckdb_duck_ident(const char *s);

/* write.c: INSERT into foreign tables */
struct ExplainState;
struct ModifyTableState;
struct ResultRelInfo;
extern char *gg_duckdb_write_target(GGForeignOptions *o, const char *unique, const char **reason);
extern int	gg_duckdb_fdw_updatable(Relation rel);
extern void gg_duckdb_fdw_begin_modify(struct ModifyTableState *mtstate, struct ResultRelInfo *rinfo,
									   List *fdw_private, int subplan_index, int eflags);
extern void gg_duckdb_fdw_begin_insert(struct ModifyTableState *mtstate, struct ResultRelInfo *rinfo);
extern TupleTableSlot *gg_duckdb_fdw_insert(EState *estate, struct ResultRelInfo *rinfo,
											TupleTableSlot *slot, TupleTableSlot *planSlot);
extern void gg_duckdb_fdw_end_modify(EState *estate, struct ResultRelInfo *rinfo);
extern void gg_duckdb_fdw_end_insert(EState *estate, struct ResultRelInfo *rinfo);
extern void gg_duckdb_fdw_explain_modify(struct ModifyTableState *mtstate, struct ResultRelInfo *rinfo,
										 List *fdw_private, int subplan_index, struct ExplainState *es);
extern bool gg_duckdb_leaf_column_type(Plan *plan, int col, GGTypeInfo *ti);
extern void gg_duckdb_numeric_state_type(GGTypeInfo *ti, int scale);
extern bool gg_duckdb_is_numeric_state(const GGTypeInfo *ti);

/* ---------- the planner pass (pass.c) ---------- */

extern void gg_duckdb_install_planner_hook(void);

#endif							/* GG_DUCKDB_H */
