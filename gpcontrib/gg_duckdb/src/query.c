/*-------------------------------------------------------------------------
 *
 * query.c
 *	  A DuckDB query driven from a plan node on the backend thread: prepare,
 *	  bind, execute task by task, stream chunks, convert rows into the
 *	  node's slot.  The region (node.c) and the native foreign scan (fdw.c)
 *	  share it.
 *
 * Nothing here touches DuckDB before gg_duckdb_query_start: the coordinator
 * initialises the nodes of every slice, including the ones it never runs,
 * and EXPLAIN without ANALYZE initialises them too.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/vmem_tracker.h"
#include "utils/faultinjector.h"
#include "executor/execExpr.h"
#include "executor/nodeSubplan.h"
#include "nodes/makefuncs.h"

#include "gg_duckdb/gg_duckdb.h"

/* Release the DuckDB objects of the current execution; safe to repeat. */
void
gg_duckdb_query_release_query(GGDuckQuery *q)
{
	if (q->chunk)
	{
		duckdb_destroy_data_chunk(&q->chunk);
		q->chunk = NULL;
	}
	if (q->has_result)
	{
		duckdb_destroy_result(&q->result);
		q->has_result = false;
	}
	if (q->pending)
		duckdb_destroy_pending(&q->pending);
	if (q->stmt)
		duckdb_destroy_prepare(&q->stmt);
	q->pending = NULL;
	q->stmt = NULL;
	q->prepared_sql = NULL;
	q->chunk_size = 0;
	q->chunk_row = 0;
}

/* The execution only: the connection and the prepared statement stay. */
static void
release_execution(GGDuckQuery *q)
{
	if (q->chunk)
	{
		duckdb_destroy_data_chunk(&q->chunk);
		q->chunk = NULL;
	}
	if (q->has_result)
	{
		duckdb_destroy_result(&q->result);
		q->has_result = false;
	}
	if (q->pending)
		duckdb_destroy_pending(&q->pending);
	q->pending = NULL;
	q->chunk_size = 0;
	q->chunk_row = 0;
}

void
gg_duckdb_query_release(GGDuckQuery *q)
{
	gg_duckdb_query_release_query(q);
	gg_duckdb_disconnect(&q->conn);
	if (q->memory_reserved > 0)
	{
		VmemTracker_ReleaseVmem(q->memory_reserved);
		q->memory_reserved = 0;
	}
}

/* Reset callback of query_cxt: an error path never leaks DuckDB objects. */
static void
query_release_callback(void *arg)
{
	gg_duckdb_query_release((GGDuckQuery *) arg);
}

void
gg_duckdb_query_init(GGDuckQuery *q, PlanState *ps, MemoryContext parent, const char *what)
{
	MemoryContextCallback *cb;

	q->ps = ps;
	q->what = what;
	q->query_cxt = AllocSetContextCreate(parent, "gg_duckdb query", ALLOCSET_DEFAULT_SIZES);
	q->chunk_cxt = AllocSetContextCreate(q->query_cxt, "gg_duckdb chunk", ALLOCSET_DEFAULT_SIZES);
	q->batch_cxt = AllocSetContextCreate(q->query_cxt, "gg_duckdb leaf batch", ALLOCSET_DEFAULT_SIZES);
	cb = MemoryContextAlloc(q->query_cxt, sizeof(MemoryContextCallback));
	cb->func = query_release_callback;
	cb->arg = q;
	MemoryContextRegisterResetCallback(q->query_cxt, cb);
}

/*
 * Output columns: the PostgreSQL side comes from the slot's tuple
 * descriptor (through colmap), the DuckDB side (type, DECIMAL width and
 * scale) from the descriptors the planner recorded; the two must agree.
 */
void
gg_duckdb_query_set_output(GGDuckQuery *q, TupleDesc desc, GGTypeInfo *outtypes,
						   int ncols, int *colmap)
{
	int			i;

	q->ncols = ncols;
	q->outtypes = outtypes;
	q->colmap = colmap;
	if (colmap == NULL && desc->natts != ncols)
		elog(ERROR, "gg_duckdb: %s has %d output descriptors for %d columns",
			 q->what, ncols, desc->natts);
	q->colvec = MemoryContextAllocZero(q->query_cxt, sizeof(duckdb_vector) * Max(ncols, 1));
	q->coldata = MemoryContextAllocZero(q->query_cxt, sizeof(void *) * Max(ncols, 1));
	q->colvalid = MemoryContextAllocZero(q->query_cxt, sizeof(uint64_t *) * Max(ncols, 1));
	q->colwidth = MemoryContextAllocZero(q->query_cxt, sizeof(int) * Max(ncols, 1));
	q->colscale = MemoryContextAllocZero(q->query_cxt, sizeof(int) * Max(ncols, 1));
	for (i = 0; i < ncols; i++)
	{
		int			attno = colmap ? colmap[i] : i;
		Form_pg_attribute att;
		GGTypeInfo	pgside;

		if (attno == -1)
			continue;			/* a placeholder column nothing receives */
		if (attno < 0 || attno >= desc->natts)
			elog(ERROR, "gg_duckdb: %s column %d maps to attribute %d of %d",
				 q->what, i + 1, attno + 1, desc->natts);
		att = TupleDescAttr(desc, attno);
		outtypes[i].typid = att->atttypid;
		outtypes[i].typmod = att->atttypmod;
		if (att->atttypid == BYTEAOID && outtypes[i].duck == DUCKDB_TYPE_STRUCT)
			continue;			/* a numeric aggregate state, packed by DuckDB */
		if (att->atttypid == NUMERICOID && att->atttypmod < (int32) VARHDRSZ)
		{
			/* unconstrained numeric: the DuckDB shape is whatever the planner emitted */
			if (outtypes[i].duck != DUCKDB_TYPE_DECIMAL)
				elog(ERROR, "gg_duckdb: %s output column %d is numeric but DuckDB returns %s",
					 q->what, i + 1, gg_duckdb_type_name(outtypes[i].duck));
			continue;
		}
		if (!gg_duckdb_type_map(att->atttypid, att->atttypmod, &pgside))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gg_duckdb: %s output column %d has type %s, which DuckDB regions do not carry",
							q->what, i + 1, format_type_with_typemod(att->atttypid, att->atttypmod))));
		if (pgside.duck != outtypes[i].duck ||
			(pgside.duck == DUCKDB_TYPE_DECIMAL &&
			 (pgside.width != outtypes[i].width || pgside.scale != outtypes[i].scale)))
			elog(ERROR, "gg_duckdb: %s output column %d: planner recorded %s, type %s maps to %s",
				 q->what, i + 1, gg_duckdb_type_name(outtypes[i].duck),
				 format_type_with_typemod(att->atttypid, att->atttypmod),
				 gg_duckdb_type_name(pgside.duck));
	}
}

/*
 * Raise the right error after DuckDB reported a failure: the PostgreSQL error
 * a leaf callback captured, if there is one, else the DuckDB message.
 */
void
gg_duckdb_query_raise(GGDuckQuery *q, const char *what, const char *msg)
{
	if (q->pending_error)
	{
		ErrorData  *edata = q->pending_error;

		q->pending_error = NULL;
		ReThrowError(edata);
	}
	gg_duckdb_note_error(msg);
	ereport(ERROR,
			(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			 errmsg("gg_duckdb: %s: %s", what, msg ? msg : "unknown error")));
}

/*
 * The value of an executor parameter the deparser turned into $n: an
 * InitPlan's result (PARAM_EXEC, dispatched to the segments, or evaluated
 * here when its plan sits in this process) or a prepared statement's
 * argument (PARAM_EXTERN), as a Const of the parameter's type.
 */
static Const *
param_value(GGDuckQuery *q, Param *param)
{
	EState	   *estate = q->ps ? q->ps->state : NULL;
	Datum		value = (Datum) 0;
	bool		isnull = true;
	int16		typlen;
	bool		typbyval;

	if (estate == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("gg_duckdb: a parameter reference outside a plan")));
	if (param->paramkind == PARAM_EXEC)
	{
		ParamExecData *prm;
		int			nexec = estate->es_plannedstmt ?
		list_length(estate->es_plannedstmt->paramExecTypes) : 0;

		if (param->paramid < 0 || param->paramid >= nexec)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("gg_duckdb: executor parameter %d is out of range", param->paramid)));
		prm = &estate->es_param_exec_vals[param->paramid];
		if (prm->execPlan != NULL)
		{
			/* an InitPlan of this process not run yet: run it, as ExecEvalParamExec does */
			ExecSetParamPlan((SubPlanState *) prm->execPlan, q->ps->ps_ExprContext, NULL);
			if (prm->execPlan != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("gg_duckdb: InitPlan of parameter %d did not run", param->paramid)));
		}
		value = prm->value;
		isnull = prm->isnull;
	}
	else if (param->paramkind == PARAM_EXTERN)
	{
		ParamListInfo params = estate->es_param_list_info;
		ParamExternData *prm;
		ParamExternData prmdata;

		if (params == NULL || param->paramid < 1 || param->paramid > params->numParams)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("gg_duckdb: no value found for parameter %d", param->paramid)));
		if (params->paramFetch != NULL)
			prm = params->paramFetch(params, param->paramid, false, &prmdata);
		else
			prm = &params->params[param->paramid - 1];
		if (!OidIsValid(prm->ptype))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("gg_duckdb: no value found for parameter %d", param->paramid)));
		if (prm->ptype != param->paramtype)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("gg_duckdb: type of parameter %d (%s) does not match that when preparing the plan (%s)",
							param->paramid, format_type_be(prm->ptype), format_type_be(param->paramtype))));
		value = prm->value;
		isnull = prm->isnull;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: unsupported parameter kind %d", (int) param->paramkind)));

	get_typlenbyval(param->paramtype, &typlen, &typbyval);
	return makeConst(param->paramtype, param->paramtypmod, param->paramcollid,
					 typlen, isnull ? (Datum) 0 : value, isnull, typbyval);
}

/* Bind the constants and executor parameters as $1..$n, the file lists after them. */
static void
bind_params(GGDuckQuery *q)
{
	ListCell   *lc;
	idx_t		idx = 1;

	foreach(lc, q->params)
	{
		Const	   *c;
		duckdb_state rc;

		if (IsA(lfirst(lc), Param))
			c = param_value(q, (Param *) lfirst(lc));
		else
			c = (Const *) lfirst(lc);

		if (c->constisnull)
			rc = duckdb_bind_null(q->stmt, idx);
		else
		{
			switch (c->consttype)
			{
				case BOOLOID:
					rc = duckdb_bind_boolean(q->stmt, idx, DatumGetBool(c->constvalue));
					break;
				case INT2OID:
					rc = duckdb_bind_int16(q->stmt, idx, DatumGetInt16(c->constvalue));
					break;
				case INT4OID:
					rc = duckdb_bind_int32(q->stmt, idx, DatumGetInt32(c->constvalue));
					break;
				case INT8OID:
					rc = duckdb_bind_int64(q->stmt, idx, DatumGetInt64(c->constvalue));
					break;
				case FLOAT4OID:
					rc = duckdb_bind_float(q->stmt, idx, DatumGetFloat4(c->constvalue));
					break;
				case FLOAT8OID:
					rc = duckdb_bind_double(q->stmt, idx, DatumGetFloat8(c->constvalue));
					break;
				case BYTEAOID:
					{
						struct varlena *v = PG_DETOAST_DATUM_PACKED(c->constvalue);

						rc = duckdb_bind_blob(q->stmt, idx, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
						break;
					}
				case INTERVALOID:
					{
						Interval   *iv = DatumGetIntervalP(c->constvalue);
						duckdb_interval d;

						d.months = iv->month;
						d.days = iv->day;
						d.micros = iv->time;
						rc = duckdb_bind_interval(q->stmt, idx, d);
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
						rc = duckdb_bind_varchar_length(q->stmt, idx, text, len);
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

	foreach(lc, q->file_lists)
	{
		List	   *files = (List *) lfirst(lc);
		int			n = list_length(files);
		duckdb_logical_type vt = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
		duckdb_value *vals = palloc(sizeof(duckdb_value) * Max(n, 1));
		duckdb_value lst;
		ListCell   *fc;
		int			i = 0;
		duckdb_state rc;

		foreach(fc, files)
			vals[i++] = duckdb_create_varchar((const char *) lfirst(fc));
		lst = duckdb_create_list_value(vt, vals, n);
		rc = duckdb_bind_value(q->stmt, idx, lst);
		duckdb_destroy_value(&lst);
		for (i = 0; i < n; i++)
			duckdb_destroy_value(&vals[i]);
		duckdb_destroy_logical_type(&vt);
		pfree(vals);
		if (rc == DuckDBError)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("gg_duckdb: could not bind the file list as parameter %d", (int) idx)));
		idx++;
	}
}

/*
 * The query's memory: the node's share of the query's memory (memquota
 * treats a region like a Sort when the planner flagged it so), capped by
 * gg_duckdb.max_memory.  Reserved with the vmem tracker up front, since the
 * tracker sees nothing of DuckDB's own allocations.
 */
static void
query_memory(GGDuckQuery *q)
{
	int64		kb = q->ps ? (int64) PlanStateOperatorMemKB(q->ps) : 0;
	int64		cap_kb = (int64) gg_duckdb_max_memory_mb * 1024;
	int64		floor_kb = (int64) gg_duckdb_min_memory_mb * 1024;
	char	   *sql;
	duckdb_result res;

	if (kb <= 0)
		kb = work_mem;
	kb = Max(kb, floor_kb);
	kb = Min(kb, cap_kb);

	if (gg_duckdb_reserve_memory && q->memory_reserved == 0)
	{
		MemoryAllocationStatus status = VmemTracker_ReserveVmem(kb * 1024);

		if (status != MemoryAllocation_Success)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("gg_duckdb: could not reserve " INT64_FORMAT " kB for a DuckDB %s (status %d)",
							kb, q->what, (int) status)));
		q->memory_reserved = kb * 1024;
	}

	sql = psprintf("SET memory_limit = '" INT64_FORMAT "KB'", kb);
	if (duckdb_query(q->conn, sql, &res) == DuckDBError)
	{
		const char *msg = duckdb_result_error(&res);
		char	   *copy = pstrdup(msg ? msg : "unknown error");

		duckdb_destroy_result(&res);
		gg_duckdb_query_raise(q, "could not set the memory limit", copy);
	}
	duckdb_destroy_result(&res);
	pfree(sql);
}

static void
run_pending(GGDuckQuery *q)
{
	int			idle = 0;

	for (;;)
	{
		duckdb_pending_state state;

		if (InterruptPending)
		{
			duckdb_interrupt(q->conn);
			CHECK_FOR_INTERRUPTS();
		}
		state = duckdb_pending_execute_task(q->pending);
		if (state == DUCKDB_PENDING_RESULT_READY)
			return;
		if (state == DUCKDB_PENDING_ERROR)
		{
			const char *msg = duckdb_pending_error(q->pending);

			gg_duckdb_query_raise(q, psprintf("%s query failed", q->what), msg ? pstrdup(msg) : NULL);
		}
		if (state == DUCKDB_PENDING_NO_TASKS_AVAILABLE)
		{
			if (++idle > 10000)
				gg_duckdb_query_raise(q, psprintf("%s query failed", q->what), "DuckDB executor made no progress");
			pg_usleep(100);
		}
		else
			idle = 0;
	}
}

/*
 * Prepare, bind and run the query to its first result.  `bctx` is the
 * leaf bind context of a region, NULL for a query without leaves.
 */
void
gg_duckdb_query_start(GGDuckQuery *q, GGBindContext *bctx)
{
	char	   *err = NULL;
	idx_t		n,
				i;
	GGBindContext *saved_bctx;

	SIMPLE_FAULT_INJECTOR("gg_duckdb_query_start");
	/*
	 * A rescan keeps the connection and, when the text is unchanged, the
	 * prepared statement: only the parameters are bound again.
	 */
	if (q->conn == NULL)
		q->conn = gg_duckdb_connect();
	if (q->stmt != NULL && (q->prepared_sql == NULL || strcmp(q->prepared_sql, q->sql) != 0))
	{
		duckdb_destroy_prepare(&q->stmt);
		q->stmt = NULL;
		q->prepared_sql = NULL;
	}
	saved_bctx = gg_duckdb_bind_context_push(bctx);

	PG_TRY();
	{
		ListCell   *lc;

		if (q->stmt == NULL)
		{
			query_memory(q);
			foreach(lc, q->pre_sql)
			{
				const char *pre = (const char *) lfirst(lc);
				duckdb_result res;

				if (duckdb_query(q->conn, pre, &res) == DuckDBError)
				{
					const char *msg = duckdb_result_error(&res);
					char	   *copy = pstrdup(msg ? msg : "unknown error");

					duckdb_destroy_result(&res);
					gg_duckdb_query_raise(q, pre, copy);
				}
				duckdb_destroy_result(&res);
			}

			q->stmt = gg_duckdb_prepare_with(bctx, q->conn, q->sql, &err);
			if (q->stmt == NULL)
				gg_duckdb_query_raise(q, psprintf("could not prepare the %s query", q->what), err);
			q->prepared_sql = MemoryContextStrdup(q->query_cxt, q->sql);
		}
		bind_params(q);

		if (duckdb_pending_prepared_streaming(q->stmt, &q->pending) == DuckDBError)
		{
			const char *msg = duckdb_pending_error(q->pending);

			gg_duckdb_query_raise(q, psprintf("could not start the %s query", q->what), msg ? pstrdup(msg) : NULL);
		}
		run_pending(q);
		if (duckdb_execute_pending(q->pending, &q->result) == DuckDBError)
		{
			const char *msg = duckdb_result_error(&q->result);
			char	   *copy = msg ? pstrdup(msg) : NULL;

			q->has_result = true;
			gg_duckdb_query_raise(q, psprintf("%s query failed", q->what), copy);
		}
		q->has_result = true;

		/*
		 * The result's columns, not the prepared statement's: a statement
		 * whose table function takes a parameter (a native reader's file
		 * list) is only bound when it executes.
		 */
		n = duckdb_column_count(&q->result);
		if ((int) n != q->ncols)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("gg_duckdb: the %s query returns %d columns, %d expected",
							q->what, (int) n, q->ncols)));
		for (i = 0; i < n; i++)
		{
			duckdb_type t = duckdb_column_type(&q->result, i);

			if (t != q->outtypes[i].duck)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("gg_duckdb: %s column %d is %s, %s expected",
								q->what, (int) i + 1, gg_duckdb_type_name(t),
								gg_duckdb_type_name(q->outtypes[i].duck))));
		}
	}
	PG_CATCH();
	{
		gg_duckdb_bind_context_pop(saved_bctx);
		gg_duckdb_query_release(q);
		PG_RE_THROW();
	}
	PG_END_TRY();
	gg_duckdb_bind_context_pop(saved_bctx);

	q->started = true;
	gg_duckdb_stats.queries_executed++;
}

/* Fetch the next chunk, or mark the query finished. */
static void
next_chunk(GGDuckQuery *q)
{
	duckdb_data_chunk chunk;
	idx_t		size;
	int			i;

	if (q->chunk)
	{
		duckdb_destroy_data_chunk(&q->chunk);
		q->chunk = NULL;
	}
	MemoryContextReset(q->chunk_cxt);

	if (InterruptPending)
	{
		duckdb_interrupt(q->conn);
		CHECK_FOR_INTERRUPTS();
	}

	chunk = duckdb_stream_fetch_chunk(q->result);
	if (chunk == NULL)
	{
		const char *err = duckdb_result_error(&q->result);

		if (q->pending_error || err)
			gg_duckdb_query_raise(q, psprintf("%s query failed", q->what), err ? pstrdup(err) : NULL);
		q->finished = true;
		return;
	}
	size = duckdb_data_chunk_get_size(chunk);
	if (size == 0)
	{
		duckdb_destroy_data_chunk(&chunk);
		if (q->pending_error)
			gg_duckdb_query_raise(q, psprintf("%s query failed", q->what), NULL);
		q->finished = true;
		return;
	}

	q->chunk = chunk;
	q->chunk_size = size;
	q->chunk_row = 0;
	q->chunks++;
	SIMPLE_FAULT_INJECTOR("gg_duckdb_after_chunk");

	for (i = 0; i < q->ncols; i++)
	{
		duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, i);
		duckdb_logical_type lt = duckdb_vector_get_column_type(vec);
		duckdb_type t = duckdb_get_type_id(lt);

		q->colwidth[i] = 0;
		q->colscale[i] = 0;
		if (t == DUCKDB_TYPE_DECIMAL)
		{
			q->colwidth[i] = duckdb_decimal_width(lt);
			q->colscale[i] = duckdb_decimal_scale(lt);
		}
		duckdb_destroy_logical_type(&lt);
		if (t != q->outtypes[i].duck ||
			(t == DUCKDB_TYPE_DECIMAL &&
			 (q->colwidth[i] != q->outtypes[i].width || q->colscale[i] != q->outtypes[i].scale)))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("gg_duckdb: %s column %d came back as %s(%d,%d), %s(%d,%d) expected",
							q->what, i + 1, gg_duckdb_type_name(t), q->colwidth[i], q->colscale[i],
							gg_duckdb_type_name(q->outtypes[i].duck),
							q->outtypes[i].width, q->outtypes[i].scale)));
		q->colvec[i] = vec;
		q->coldata[i] = duckdb_vector_get_data(vec);
		q->colvalid[i] = duckdb_vector_get_validity(vec);
	}
}

/*
 * The next row into `slot` (a virtual tuple); false at the end.  The caller
 * started the query.
 */
bool
gg_duckdb_query_next(GGDuckQuery *q, TupleTableSlot *slot)
{
	for (;;)
	{
		if (q->finished)
		{
			ExecClearTuple(slot);
			return false;
		}
		if (q->chunk != NULL && q->chunk_row < q->chunk_size)
		{
			MemoryContext oldcxt = MemoryContextSwitchTo(q->chunk_cxt);
			int			i;

			ExecClearTuple(slot);
			if (q->colmap)
			{
				int			natts = slot->tts_tupleDescriptor->natts;

				for (i = 0; i < natts; i++)
				{
					slot->tts_values[i] = (Datum) 0;
					slot->tts_isnull[i] = true;
				}
			}
			for (i = 0; i < q->ncols; i++)
			{
				int			attno = q->colmap ? q->colmap[i] : i;

				if (attno == -1)
					continue;
				slot->tts_values[attno] = gg_duckdb_read_datum(&q->outtypes[i],
															   q->outtypes[i].duck,
															   q->colwidth[i], q->colscale[i],
															   q->colvec[i],
															   q->coldata[i], q->colvalid[i],
															   q->chunk_row, &slot->tts_isnull[attno]);
			}
			MemoryContextSwitchTo(oldcxt);
			ExecStoreVirtualTuple(slot);
			q->chunk_row++;
			q->rows_out++;
			return true;
		}
		next_chunk(q);
	}
}

/* Forget the current execution so that the query can be started again. */
void
gg_duckdb_query_reset(GGDuckQuery *q)
{
	release_execution(q);
	q->started = false;
	q->finished = false;
	q->pending_error = NULL;
	q->file_lists = NIL;
}

/* EXPLAIN ANALYZE text from the QE that ran the query. */
void
gg_duckdb_query_explain_end(GGDuckQuery *q, StringInfo buf)
{
	appendStringInfo(buf, "DuckDB: " INT64_FORMAT " rows out in " INT64_FORMAT " chunks",
					 q->rows_out, q->chunks);
}
