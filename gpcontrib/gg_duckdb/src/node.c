/*-------------------------------------------------------------------------
 *
 * node.c
 *	  The DuckDB region: a CustomScan whose interior runs in the embedded
 *	  DuckDB and whose leaves are ordinary plan nodes kept in custom_ps.
 *
 * Nothing here touches DuckDB before the first ExecCustomScan call: the
 * coordinator initialises the nodes of every slice, including the ones it
 * never runs, and EXPLAIN without ANALYZE initialises them too.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "lib/stringinfo.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "gg_duckdb/gg_duckdb.h"

static Node *region_create_state(CustomScan *cscan);
static void region_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *region_exec(CustomScanState *node);
static void region_end(CustomScanState *node);
static void region_rescan(CustomScanState *node);
static void region_shutdown(CustomScanState *node);
static void region_explain(CustomScanState *node, List *ancestors, ExplainState *es);
static void region_explain_end(PlanState *planstate, struct StringInfoData *buf);

CustomScanMethods gg_duckdb_scan_methods = {
	GG_DUCKDB_REGION_NAME,
	region_create_state
};

static CustomExecMethods region_exec_methods = {
	GG_DUCKDB_REGION_NAME,
	region_begin,
	region_exec,
	region_end,
	region_rescan,
	NULL,						/* MarkPos */
	NULL,						/* RestrPos */
	NULL,						/* EstimateDSM */
	NULL,						/* InitializeDSM */
	NULL,						/* ReInitializeDSM */
	NULL,						/* InitializeWorker */
	region_shutdown,
	region_explain
};

/* ---------- custom_private ---------- */

static Const *
make_int_const(int v)
{
	return makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(v), false, true);
}

static Const *
make_text_const(const char *s)
{
	return makeConst(TEXTOID, -1, InvalidOid, -1, PointerGetDatum(cstring_to_text(s)), false, false);
}

List *
gg_duckdb_make_private(const char *sql, int flags, int nleaves, const char *label)
{
	List	   *priv = NIL;

	priv = lappend(priv, make_int_const(GG_DUCKDB_PRIVATE_VERSION));
	priv = lappend(priv, make_text_const(sql));
	priv = lappend(priv, make_int_const(flags));
	priv = lappend(priv, make_int_const(nleaves));
	priv = lappend(priv, make_text_const(label));
	priv = lappend(priv, make_int_const(0));	/* params: none yet */
	return priv;
}

static int
private_int(List *priv, int idx)
{
	Const	   *c = list_nth_node(Const, priv, idx);

	return DatumGetInt32(c->constvalue);
}

static char *
private_text(List *priv, int idx)
{
	Const	   *c = list_nth_node(Const, priv, idx);

	return TextDatumGetCString(c->constvalue);
}

/* ---------- lifecycle ---------- */

static Node *
region_create_state(CustomScan *cscan)
{
	GGRegionState *st = (GGRegionState *) newNode(sizeof(GGRegionState), T_CustomScanState);
	List	   *priv = cscan->custom_private;

	st->css.methods = &region_exec_methods;

	if (list_length(priv) < GGP_NFIELDS)
		elog(ERROR, "gg_duckdb: malformed region node");
	st->version = private_int(priv, GGP_VERSION);
	if (st->version != GG_DUCKDB_PRIVATE_VERSION)
		elog(ERROR, "gg_duckdb: region node version %d, expected %d",
			 st->version, GG_DUCKDB_PRIVATE_VERSION);
	st->sql = private_text(priv, GGP_SQL);
	st->flags = private_int(priv, GGP_FLAGS);
	st->nleaves = private_int(priv, GGP_NLEAVES);
	st->label = private_text(priv, GGP_LABEL);

	return (Node *) st;
}

/* Release the DuckDB objects of the current execution; safe to repeat. */
static void
region_release_query(GGRegionState *st)
{
	if (st->chunk)
	{
		duckdb_destroy_data_chunk(&st->chunk);
		st->chunk = NULL;
	}
	if (st->has_result)
	{
		duckdb_destroy_result(&st->result);
		st->has_result = false;
	}
	if (st->pending)
		duckdb_destroy_pending(&st->pending);
	if (st->stmt)
		duckdb_destroy_prepare(&st->stmt);
	st->pending = NULL;
	st->stmt = NULL;
	st->chunk_size = 0;
	st->chunk_row = 0;
}

static void
region_release(GGRegionState *st)
{
	region_release_query(st);
	gg_duckdb_disconnect(&st->conn);
}

/* Reset callback of region_cxt: an error path never leaks DuckDB objects. */
static void
region_release_callback(void *arg)
{
	region_release((GGRegionState *) arg);
}

static void
describe_leaf(GGRegionState *st, GGLeaf *lf, PlanState *ps, int leafno)
{
	TupleDesc	desc = ExecGetResultType(ps);
	int			k;

	lf->ps = ps;
	lf->ncols = desc->natts;
	lf->cols = MemoryContextAllocZero(st->region_cxt, sizeof(GGLeafCol) * Max(desc->natts, 1));
	lf->plan_rows = ps->plan->plan_rows;
	for (k = 0; k < desc->natts; k++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, k);

		snprintf(lf->cols[k].name, sizeof(lf->cols[k].name), "c%d", k + 1);
		if (!gg_duckdb_type_map(att->atttypid, att->atttypmod, &lf->cols[k].type))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gg_duckdb: leaf %d column %d has type %s, which DuckDB regions do not carry",
							leafno, k + 1, format_type_with_typemod(att->atttypid, att->atttypmod))));
	}
}

static void
region_begin(CustomScanState *node, EState *estate, int eflags)
{
	GGRegionState *st = (GGRegionState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	MemoryContextCallback *cb;
	TupleDesc	desc;
	ListCell   *lc;
	int			i;

	st->region_cxt = AllocSetContextCreate(estate->es_query_cxt,
										   "gg_duckdb region",
										   ALLOCSET_DEFAULT_SIZES);
	st->chunk_cxt = AllocSetContextCreate(st->region_cxt,
										  "gg_duckdb chunk",
										  ALLOCSET_DEFAULT_SIZES);
	st->batch_cxt = AllocSetContextCreate(st->region_cxt,
										  "gg_duckdb leaf batch",
										  ALLOCSET_DEFAULT_SIZES);
	cb = MemoryContextAlloc(st->region_cxt, sizeof(MemoryContextCallback));
	cb->func = region_release_callback;
	cb->arg = st;
	MemoryContextRegisterResetCallback(st->region_cxt, cb);

	if (list_length(cscan->custom_plans) != st->nleaves)
		elog(ERROR, "gg_duckdb: region declares %d leaves but carries %d",
			 st->nleaves, list_length(cscan->custom_plans));
	st->leaves = MemoryContextAllocZero(st->region_cxt, sizeof(GGLeaf) * Max(st->nleaves, 1));

	/*
	 * The leaves are initialised even for EXPLAIN, so that they are printed;
	 * a region never scans backwards nor marks positions.
	 */
	i = 0;
	foreach(lc, cscan->custom_plans)
	{
		Plan	   *plan = (Plan *) lfirst(lc);
		PlanState  *ps = ExecInitNode(plan, estate,
									  eflags & ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK));

		node->custom_ps = lappend(node->custom_ps, ps);
		describe_leaf(st, &st->leaves[i], ps, i);
		i++;
	}

	/* per-node text for EXPLAIN ANALYZE, collected from every QE */
	if (estate->es_instrument)
	{
		node->ss.ps.cdbexplainbuf = makeStringInfo();
		node->ss.ps.cdbexplainfun = region_explain_end;
	}

	desc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	st->ncols = desc->natts;
	st->outtypes = MemoryContextAllocZero(st->region_cxt, sizeof(GGTypeInfo) * Max(desc->natts, 1));
	st->coldata = MemoryContextAllocZero(st->region_cxt, sizeof(void *) * Max(desc->natts, 1));
	st->colvalid = MemoryContextAllocZero(st->region_cxt, sizeof(uint64_t *) * Max(desc->natts, 1));
	st->colwidth = MemoryContextAllocZero(st->region_cxt, sizeof(int) * Max(desc->natts, 1));
	st->colscale = MemoryContextAllocZero(st->region_cxt, sizeof(int) * Max(desc->natts, 1));
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!gg_duckdb_type_map(att->atttypid, att->atttypmod, &st->outtypes[i]))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gg_duckdb: region output column %d has type %s, which DuckDB regions do not carry",
							i + 1, format_type_with_typemod(att->atttypid, att->atttypmod))));
	}
}

/*
 * Raise the right error after DuckDB reported a failure: the PostgreSQL error
 * a leaf callback captured, if there is one, else the DuckDB message.
 */
static void
region_raise(GGRegionState *st, const char *what, const char *msg)
{
	if (st->pending_error)
	{
		ErrorData  *edata = st->pending_error;

		st->pending_error = NULL;
		ReThrowError(edata);
	}
	gg_duckdb_note_error(msg);
	ereport(ERROR,
			(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			 errmsg("gg_duckdb: %s: %s", what, msg ? msg : "unknown error")));
}

static void
region_run_pending(GGRegionState *st)
{
	int			idle = 0;

	for (;;)
	{
		duckdb_pending_state state;

		if (InterruptPending)
		{
			duckdb_interrupt(st->conn);
			CHECK_FOR_INTERRUPTS();
		}
		state = duckdb_pending_execute_task(st->pending);
		if (state == DUCKDB_PENDING_RESULT_READY)
			return;
		if (state == DUCKDB_PENDING_ERROR)
		{
			const char *msg = duckdb_pending_error(st->pending);

			region_raise(st, "region query failed", msg ? pstrdup(msg) : NULL);
		}
		if (state == DUCKDB_PENDING_NO_TASKS_AVAILABLE)
		{
			if (++idle > 10000)
				region_raise(st, "region query failed", "DuckDB executor made no progress");
			pg_usleep(100);
		}
		else
			idle = 0;
	}
}

static void
region_start(GGRegionState *st)
{
	char	   *err = NULL;
	idx_t		n,
				i;

	st->conn = gg_duckdb_connect();

	PG_TRY();
	{
		st->stmt = gg_duckdb_prepare_region(st, st->conn, st->sql, &err);
		if (st->stmt == NULL)
			region_raise(st, "could not prepare the region query", err);

		n = duckdb_prepared_statement_column_count(st->stmt);
		if ((int) n != st->ncols)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("gg_duckdb: the region query returns %d columns, %d expected",
							(int) n, st->ncols)));
		for (i = 0; i < n; i++)
		{
			duckdb_type t = duckdb_prepared_statement_column_type(st->stmt, i);

			if (t != st->outtypes[i].duck)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("gg_duckdb: region column %d is %s, %s expected",
								(int) i + 1, gg_duckdb_type_name(t),
								gg_duckdb_type_name(st->outtypes[i].duck))));
		}

		if (duckdb_pending_prepared_streaming(st->stmt, &st->pending) == DuckDBError)
		{
			const char *msg = duckdb_pending_error(st->pending);

			region_raise(st, "could not start the region query", msg ? pstrdup(msg) : NULL);
		}
		region_run_pending(st);
		if (duckdb_execute_pending(st->pending, &st->result) == DuckDBError)
		{
			const char *msg = duckdb_result_error(&st->result);
			char	   *copy = msg ? pstrdup(msg) : NULL;

			st->has_result = true;
			region_raise(st, "region query failed", copy);
		}
		st->has_result = true;
	}
	PG_CATCH();
	{
		region_release(st);
		PG_RE_THROW();
	}
	PG_END_TRY();

	st->started = true;
	gg_duckdb_stats.queries_executed++;
}

/* Fetch the next chunk, or mark the region finished. */
static void
region_next_chunk(GGRegionState *st)
{
	duckdb_data_chunk chunk;
	idx_t		size;
	int			i;

	if (st->chunk)
	{
		duckdb_destroy_data_chunk(&st->chunk);
		st->chunk = NULL;
	}
	MemoryContextReset(st->chunk_cxt);

	if (InterruptPending)
	{
		duckdb_interrupt(st->conn);
		CHECK_FOR_INTERRUPTS();
	}

	chunk = duckdb_stream_fetch_chunk(st->result);
	if (chunk == NULL)
	{
		const char *err = duckdb_result_error(&st->result);

		if (st->pending_error || err)
			region_raise(st, "region query failed", err ? pstrdup(err) : NULL);
		st->finished = true;
		return;
	}
	size = duckdb_data_chunk_get_size(chunk);
	if (size == 0)
	{
		duckdb_destroy_data_chunk(&chunk);
		if (st->pending_error)
			region_raise(st, "region query failed", NULL);
		st->finished = true;
		return;
	}

	st->chunk = chunk;
	st->chunk_size = size;
	st->chunk_row = 0;
	st->chunks++;

	for (i = 0; i < st->ncols; i++)
	{
		duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, i);
		duckdb_logical_type lt = duckdb_vector_get_column_type(vec);
		duckdb_type t = duckdb_get_type_id(lt);

		st->colwidth[i] = 0;
		st->colscale[i] = 0;
		if (t == DUCKDB_TYPE_DECIMAL)
		{
			st->colwidth[i] = duckdb_decimal_width(lt);
			st->colscale[i] = duckdb_decimal_scale(lt);
		}
		duckdb_destroy_logical_type(&lt);
		if (t != st->outtypes[i].duck ||
			(t == DUCKDB_TYPE_DECIMAL &&
			 (st->colwidth[i] != st->outtypes[i].width || st->colscale[i] != st->outtypes[i].scale)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("gg_duckdb: region column %d came back as %s(%d,%d), %s(%d,%d) expected",
							i + 1, gg_duckdb_type_name(t), st->colwidth[i], st->colscale[i],
							gg_duckdb_type_name(st->outtypes[i].duck),
							st->outtypes[i].width, st->outtypes[i].scale)));
		st->coldata[i] = duckdb_vector_get_data(vec);
		st->colvalid[i] = duckdb_vector_get_validity(vec);
	}
}

static TupleTableSlot *
region_exec(CustomScanState *node)
{
	GGRegionState *st = (GGRegionState *) node;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	if (st->finished)
		return ExecClearTuple(slot);
	if (!st->started)
		region_start(st);

	for (;;)
	{
		if (st->chunk != NULL && st->chunk_row < st->chunk_size)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(st->chunk_cxt);
			int			i;

			ExecClearTuple(slot);
			for (i = 0; i < st->ncols; i++)
				slot->tts_values[i] = gg_duckdb_read_datum(&st->outtypes[i],
														   st->outtypes[i].duck,
														   st->colwidth[i], st->colscale[i],
														   st->coldata[i], st->colvalid[i],
														   st->chunk_row, &slot->tts_isnull[i]);
			MemoryContextSwitchTo(oldcxt);
			ExecStoreVirtualTuple(slot);
			st->chunk_row++;
			st->rows_out++;
			return slot;
		}
		if (st->finished)
			return ExecClearTuple(slot);
		region_next_chunk(st);
	}
}

static void
region_end(CustomScanState *node)
{
	GGRegionState *st = (GGRegionState *) node;
	ListCell   *lc;

	region_release(st);
	foreach(lc, node->custom_ps)
		ExecEndNode((PlanState *) lfirst(lc));
	if (gg_duckdb_release_instance_at_end)
		gg_duckdb_instance_close();
}

static void
region_rescan(CustomScanState *node)
{
	GGRegionState *st = (GGRegionState *) node;
	ListCell   *lc;
	int			i;

	region_release_query(st);
	st->started = false;
	st->finished = false;
	st->pending_error = NULL;

	i = 0;
	foreach(lc, node->custom_ps)
	{
		PlanState  *child = (PlanState *) lfirst(lc);

		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(child, node->ss.ps.chgParam);
		ExecReScan(child);
		st->leaves[i].eof = false;
		st->leaves[i].rows = 0;
		i++;
	}
}

/* Squelch / end of plan: stop the DuckDB query, keep nothing running. */
static void
region_shutdown(CustomScanState *node)
{
	GGRegionState *st = (GGRegionState *) node;

	region_release_query(st);
	st->finished = true;
}

/* EXPLAIN ANALYZE text from the QE that ran the region. */
static void
region_explain_end(PlanState *planstate, struct StringInfoData *buf)
{
	GGRegionState *st = (GGRegionState *) planstate;
	int			i;

	if (!st->started && st->rows_out == 0)
		return;
	appendStringInfo(buf, "DuckDB: " INT64_FORMAT " rows out in " INT64_FORMAT " chunks",
					 st->rows_out, st->chunks);
	for (i = 0; i < st->nleaves; i++)
		appendStringInfo(buf, "%s leaf %d: " INT64_FORMAT " rows in",
						 i == 0 ? ";" : ",", i, st->leaves[i].rows);
	appendStringInfoChar(buf, '\n');
}

static void
region_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GGRegionState *st = (GGRegionState *) node;

	ExplainPropertyText("DuckDB", st->label, es);
	if (es->verbose)
		ExplainPropertyText("DuckDB SQL", st->sql, es);
}
