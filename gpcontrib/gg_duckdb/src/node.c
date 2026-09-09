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
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/vmem_tracker.h"

#include "gg_duckdb/gg_duckdb.h"

static Node *region_create_state(CustomScan *cscan);
static void region_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *region_exec(CustomScanState *node);
static void region_end(CustomScanState *node);
static void region_rescan(CustomScanState *node);
static void region_shutdown(CustomScanState *node);
static void region_explain(CustomScanState *node, List *ancestors, ExplainState *es);
static void region_explain_end(PlanState *planstate, struct StringInfoData *buf);
static void region_raise(GGRegionState *st, const char *what, const char *msg) pg_attribute_noreturn();

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

/*
 * Layout (a List of Const): version, sql, flags, nleaves, label, nparams,
 * params..., nout, output descriptors..., then per leaf its column count
 * and column descriptors.  The leaf shapes are what the coordinator's
 * deparser assumed; a segment cannot re-derive them once the subtree below
 * a leaf has itself become a region.
 */
List *
gg_duckdb_make_private(const char *sql, int flags, List *leaves, const char *label,
					   List *params, int nout, const GGTypeInfo *outtypes)
{
	List	   *priv = NIL;
	ListCell   *lc;
	int			i;

	priv = lappend(priv, make_int_const(GG_DUCKDB_PRIVATE_VERSION));
	priv = lappend(priv, make_text_const(sql));
	priv = lappend(priv, make_int_const(flags));
	priv = lappend(priv, make_int_const(list_length(leaves)));
	priv = lappend(priv, make_text_const(label));
	priv = lappend(priv, make_int_const(list_length(params)));
	foreach(lc, params)
		priv = lappend(priv, copyObject(lfirst(lc)));
	priv = lappend(priv, make_int_const(nout));
	for (i = 0; i < nout; i++)
		priv = lappend(priv, make_int_const(GG_OUTDESC(&outtypes[i])));
	foreach(lc, leaves)
	{
		GGLeafDesc	desc;

		gg_duckdb_describe_leaf_plan(&desc, (Plan *) lfirst(lc));
		priv = lappend(priv, make_int_const(desc.ncols));
		for (i = 0; i < desc.ncols; i++)
			priv = lappend(priv, make_int_const(GG_OUTDESC(&desc.cols[i].type)));
	}
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
	{
		int			nparams = private_int(priv, GGP_NPARAMS);
		int			pos = GGP_NPARAMS + 1;
		int			nout,
					i;

		if (list_length(priv) < pos + nparams + 1)
			elog(ERROR, "gg_duckdb: malformed region node");
		for (i = 0; i < nparams; i++)
			st->params = lappend(st->params, list_nth(priv, pos + i));
		pos += nparams;
		nout = private_int(priv, pos);
		pos++;
		if (list_length(priv) < pos + nout)
			elog(ERROR, "gg_duckdb: malformed region node");
		st->ncols = nout;
		st->outtypes = palloc0(sizeof(GGTypeInfo) * Max(nout, 1));
		for (i = 0; i < nout; i++)
		{
			int			d = private_int(priv, pos + i);

			st->outtypes[i].duck = (duckdb_type) (d >> 16);
			st->outtypes[i].width = (uint8) ((d >> 8) & 0xff);
			st->outtypes[i].scale = (uint8) (d & 0xff);
		}
		pos += nout;
		st->leaf_shapes = palloc0(sizeof(GGTypeInfo *) * Max(st->nleaves, 1));
		st->leaf_ncols = palloc0(sizeof(int) * Max(st->nleaves, 1));
		for (i = 0; i < st->nleaves; i++)
		{
			int			ncols,
						k;

			if (list_length(priv) < pos + 1)
				elog(ERROR, "gg_duckdb: malformed region node");
			ncols = private_int(priv, pos);
			pos++;
			if (list_length(priv) < pos + ncols)
				elog(ERROR, "gg_duckdb: malformed region node");
			st->leaf_ncols[i] = ncols;
			st->leaf_shapes[i] = palloc0(sizeof(GGTypeInfo) * Max(ncols, 1));
			for (k = 0; k < ncols; k++)
			{
				int			d = private_int(priv, pos + k);

				st->leaf_shapes[i][k].duck = (duckdb_type) (d >> 16);
				st->leaf_shapes[i][k].width = (uint8) ((d >> 8) & 0xff);
				st->leaf_shapes[i][k].scale = (uint8) (d & 0xff);
			}
			pos += ncols;
		}
	}

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
	if (st->memory_reserved > 0)
	{
		VmemTracker_ReleaseVmem(st->memory_reserved);
		st->memory_reserved = 0;
	}
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
	lf->desc.ncols = desc->natts;
	lf->desc.cols = MemoryContextAllocZero(st->region_cxt, sizeof(GGLeafCol) * Max(desc->natts, 1));
	lf->desc.plan_rows = ps->plan->plan_rows;
	if (desc->natts != st->leaf_ncols[leafno])
		elog(ERROR, "gg_duckdb: leaf %d returns %d columns, the planner recorded %d",
			 leafno, desc->natts, st->leaf_ncols[leafno]);
	for (k = 0; k < desc->natts; k++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, k);
		GGTypeInfo *ti = &lf->desc.cols[k].type;
		GGTypeInfo	pgside;

		snprintf(lf->desc.cols[k].name, sizeof(lf->desc.cols[k].name), "c%d", k + 1);
		/* the shape the coordinator's deparser assumed, checked against the type */
		*ti = st->leaf_shapes[leafno][k];
		ti->typid = att->atttypid;
		ti->typmod = att->atttypmod;
		if (gg_duckdb_is_numeric_state(ti))
			continue;
		if (!gg_duckdb_type_map(att->atttypid, att->atttypmod, &pgside))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gg_duckdb: leaf %d column %d has type %s, which DuckDB regions do not carry",
							leafno, k + 1, format_type_with_typemod(att->atttypid, att->atttypmod))));
		if (pgside.duck != ti->duck ||
			(pgside.duck == DUCKDB_TYPE_DECIMAL &&
			 (pgside.width != ti->width || pgside.scale != ti->scale)))
			elog(ERROR, "gg_duckdb: leaf %d column %d: planner recorded %s, type %s maps to %s",
				 leafno, k + 1, gg_duckdb_type_name(ti->duck),
				 format_type_with_typemod(att->atttypid, att->atttypmod),
				 gg_duckdb_type_name(pgside.duck));
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

	/*
	 * Output columns: the PostgreSQL side comes from the scan tuple
	 * descriptor, the DuckDB side (type, DECIMAL width and scale) from the
	 * descriptors the planner recorded; the two must agree.
	 */
	desc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	if (desc->natts != st->ncols)
		elog(ERROR, "gg_duckdb: region has %d output descriptors for %d columns",
			 st->ncols, desc->natts);
	st->colvec = MemoryContextAllocZero(st->region_cxt, sizeof(duckdb_vector) * Max(desc->natts, 1));
	st->coldata = MemoryContextAllocZero(st->region_cxt, sizeof(void *) * Max(desc->natts, 1));
	st->colvalid = MemoryContextAllocZero(st->region_cxt, sizeof(uint64_t *) * Max(desc->natts, 1));
	st->colwidth = MemoryContextAllocZero(st->region_cxt, sizeof(int) * Max(desc->natts, 1));
	st->colscale = MemoryContextAllocZero(st->region_cxt, sizeof(int) * Max(desc->natts, 1));
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		GGTypeInfo	pgside;

		st->outtypes[i].typid = att->atttypid;
		st->outtypes[i].typmod = att->atttypmod;
		if (att->atttypid == BYTEAOID && st->outtypes[i].duck == DUCKDB_TYPE_STRUCT)
			continue;			/* a numeric aggregate state, packed by DuckDB */
		if (att->atttypid == NUMERICOID && att->atttypmod < (int32) VARHDRSZ)
		{
			/* unconstrained numeric: the DuckDB shape is whatever the planner emitted */
			if (st->outtypes[i].duck != DUCKDB_TYPE_DECIMAL)
				elog(ERROR, "gg_duckdb: region output column %d is numeric but DuckDB returns %s",
					 i + 1, gg_duckdb_type_name(st->outtypes[i].duck));
			continue;
		}
		if (!gg_duckdb_type_map(att->atttypid, att->atttypmod, &pgside))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gg_duckdb: region output column %d has type %s, which DuckDB regions do not carry",
							i + 1, format_type_with_typemod(att->atttypid, att->atttypmod))));
		if (pgside.duck != st->outtypes[i].duck ||
			(pgside.duck == DUCKDB_TYPE_DECIMAL &&
			 (pgside.width != st->outtypes[i].width || pgside.scale != st->outtypes[i].scale)))
			elog(ERROR, "gg_duckdb: region output column %d: planner recorded %s, type %s maps to %s",
				 i + 1, gg_duckdb_type_name(st->outtypes[i].duck),
				 format_type_with_typemod(att->atttypid, att->atttypmod),
				 gg_duckdb_type_name(pgside.duck));
	}
}

/* Bind the region's constants as $1..$n. */
static void
bind_params(GGRegionState *st)
{
	ListCell   *lc;
	idx_t		idx = 1;

	foreach(lc, st->params)
	{
		Const	   *c = (Const *) lfirst(lc);
		duckdb_state rc;

		if (c->constisnull)
			rc = duckdb_bind_null(st->stmt, idx);
		else
		{
			switch (c->consttype)
			{
				case BOOLOID:
					rc = duckdb_bind_boolean(st->stmt, idx, DatumGetBool(c->constvalue));
					break;
				case INT2OID:
					rc = duckdb_bind_int16(st->stmt, idx, DatumGetInt16(c->constvalue));
					break;
				case INT4OID:
					rc = duckdb_bind_int32(st->stmt, idx, DatumGetInt32(c->constvalue));
					break;
				case INT8OID:
					rc = duckdb_bind_int64(st->stmt, idx, DatumGetInt64(c->constvalue));
					break;
				case FLOAT4OID:
					rc = duckdb_bind_float(st->stmt, idx, DatumGetFloat4(c->constvalue));
					break;
				case FLOAT8OID:
					rc = duckdb_bind_double(st->stmt, idx, DatumGetFloat8(c->constvalue));
					break;
				case BYTEAOID:
					{
						struct varlena *v = PG_DETOAST_DATUM_PACKED(c->constvalue);

						rc = duckdb_bind_blob(st->stmt, idx, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
						break;
					}
				case INTERVALOID:
					{
						Interval   *iv = DatumGetIntervalP(c->constvalue);
						duckdb_interval d;

						d.months = iv->month;
						d.days = iv->day;
						d.micros = iv->time;
						rc = duckdb_bind_interval(st->stmt, idx, d);
						break;
					}
				default:
					{
						/* everything else through its text form and the query's CAST */
						Oid			typoutput;
						bool		isvarlena;
						char	   *text;
						int			len;

						getTypeOutputInfo(c->consttype, &typoutput, &isvarlena);
						text = OidOutputFunctionCall(typoutput, c->constvalue);
						len = strlen(text);
						if (c->consttype == BPCHAROID)
							len = bpchartruelen(text, len);
						rc = duckdb_bind_varchar_length(st->stmt, idx, text, len);
						break;
					}
			}
		}
		if (rc == DuckDBError)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("gg_duckdb: could not bind parameter %d", (int) idx)));
		idx++;
	}
}

/*
 * The region's memory: its share of the query's memory (memquota treats a
 * region like a Sort when the planner flagged it so), capped by
 * gg_duckdb.max_memory.  Reserved with the vmem tracker up front, since the
 * tracker sees nothing of DuckDB's own allocations.
 */
static void
region_memory(GGRegionState *st)
{
	int64		kb = (int64) PlanStateOperatorMemKB(&st->css.ss.ps);
	int64		cap_kb = (int64) gg_duckdb_max_memory_mb * 1024;
	int64		floor_kb = (int64) gg_duckdb_min_memory_mb * 1024;
	char	   *sql;
	duckdb_result res;

	if (kb <= 0)
		kb = work_mem;
	kb = Max(kb, floor_kb);
	kb = Min(kb, cap_kb);

	if (gg_duckdb_reserve_memory && st->memory_reserved == 0)
	{
		MemoryAllocationStatus status = VmemTracker_ReserveVmem(kb * 1024);

		if (status != MemoryAllocation_Success)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("gg_duckdb: could not reserve " INT64_FORMAT " kB for a DuckDB region (status %d)",
							kb, (int) status)));
		st->memory_reserved = kb * 1024;
	}

	sql = psprintf("SET memory_limit = '" INT64_FORMAT "KB'", kb);
	if (duckdb_query(st->conn, sql, &res) == DuckDBError)
	{
		const char *msg = duckdb_result_error(&res);
		char	   *copy = pstrdup(msg ? msg : "unknown error");

		duckdb_destroy_result(&res);
		region_raise(st, "could not set the memory limit", copy);
	}
	duckdb_destroy_result(&res);
	pfree(sql);
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

	GGBindContext bctx;
	GGBindContext *saved_bctx;
	GGLeafDesc **descs = palloc(sizeof(GGLeafDesc *) * Max(st->nleaves, 1));

	for (i = 0; i < (idx_t) st->nleaves; i++)
		descs[i] = &st->leaves[i].desc;
	bctx.region = st;
	bctx.nleaves = st->nleaves;
	bctx.leaves = descs;

	st->conn = gg_duckdb_connect();
	saved_bctx = gg_duckdb_bind_context_push(&bctx);

	PG_TRY();
	{
		region_memory(st);

		st->stmt = gg_duckdb_prepare_with(&bctx, st->conn, st->sql, &err);
		if (st->stmt == NULL)
			region_raise(st, "could not prepare the region query", err);
		bind_params(st);

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
		gg_duckdb_bind_context_pop(saved_bctx);
		region_release(st);
		PG_RE_THROW();
	}
	PG_END_TRY();
	gg_duckdb_bind_context_pop(saved_bctx);

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
		st->colvec[i] = vec;
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
														   st->colvec[i],
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
