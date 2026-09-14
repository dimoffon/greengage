/*-------------------------------------------------------------------------
 *
 * funcs.c
 *	  SQL-callable functions of gg_duckdb: version(), query(), status().
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/htup_details.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "gg_duckdb/gg_duckdb.h"

#ifndef GG_DUCKDB_PINNED_VERSION
#define GG_DUCKDB_PINNED_VERSION "unknown"
#endif

PG_FUNCTION_INFO_V1(gg_duckdb_version);
PG_FUNCTION_INFO_V1(gg_duckdb_query);
PG_FUNCTION_INFO_V1(gg_duckdb_status);

Datum
gg_duckdb_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(duckdb_library_version()));
}

/*
 * State of one gg_duckdb.query() call: the materialised DuckDB result and the
 * connection it came from, freed by a reset callback on the SRF's memory
 * context so that an early stop (LIMIT) releases them too.
 */
typedef struct QueryState
{
	duckdb_connection conn;
	duckdb_result result;
	bool		has_result;
	idx_t		nrows;
	idx_t		ncols;
	idx_t		row;
} QueryState;

static void
query_state_release(void *arg)
{
	QueryState *st = (QueryState *) arg;

	if (st->has_result)
	{
		duckdb_destroy_result(&st->result);
		st->has_result = false;
	}
	gg_duckdb_disconnect(&st->conn);
	if (gg_duckdb_release_instance_at_end)
		gg_duckdb_instance_close();
}

static void
query_state_run(QueryState *st, const char *sql)
{
	duckdb_prepared_statement stmt = NULL;
	duckdb_pending_result pending = NULL;

	st->conn = gg_duckdb_connect();

	PG_TRY();
	{
		if (duckdb_prepare(st->conn, sql, &stmt) == DuckDBError)
		{
			const char *msg = duckdb_prepare_error(stmt);

			gg_duckdb_note_error(msg);
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("gg_duckdb: %s", msg ? msg : "could not prepare statement")));
		}
		if (duckdb_pending_prepared(stmt, &pending) == DuckDBError)
		{
			const char *msg = duckdb_pending_error(pending);

			gg_duckdb_note_error(msg);
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("gg_duckdb: %s", msg ? msg : "could not start statement")));
		}

		gg_duckdb_run_pending(st->conn, pending);

		if (duckdb_execute_pending(pending, &st->result) == DuckDBError)
		{
			const char *msg = duckdb_result_error(&st->result);
			char	   *copy = pstrdup(msg ? msg : "query failed");

			duckdb_destroy_result(&st->result);
			gg_duckdb_note_error(copy);
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("gg_duckdb: %s", copy)));
		}
		st->has_result = true;
	}
	PG_CATCH();
	{
		if (pending)
			duckdb_destroy_pending(&pending);
		if (stmt)
			duckdb_destroy_prepare(&stmt);
		PG_RE_THROW();
	}
	PG_END_TRY();

	duckdb_destroy_pending(&pending);
	duckdb_destroy_prepare(&stmt);

	st->nrows = duckdb_row_count(&st->result);
	st->ncols = duckdb_column_count(&st->result);
	st->row = 0;
	gg_duckdb_stats.queries_executed++;
}

Datum
gg_duckdb_query(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	QueryState *st;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		MemoryContextCallback *cb;
		char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		st = palloc0(sizeof(QueryState));
		cb = palloc0(sizeof(MemoryContextCallback));
		cb->func = query_state_release;
		cb->arg = st;
		MemoryContextRegisterResetCallback(funcctx->multi_call_memory_ctx, cb);
		funcctx->user_fctx = st;

		query_state_run(st, sql);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	st = (QueryState *) funcctx->user_fctx;

	if (st->row < st->nrows)
	{
		StringInfoData line;
		idx_t		col;

		initStringInfo(&line);
		for (col = 0; col < st->ncols; col++)
		{
			char	   *val = duckdb_value_varchar(&st->result, col, st->row);

			if (col > 0)
				appendStringInfoChar(&line, '|');
			appendStringInfoString(&line, val ? val : "NULL");
			if (val)
				duckdb_free(val);
		}
		st->row++;
		SRF_RETURN_NEXT(funcctx, PointerGetDatum(cstring_to_text(line.data)));
	}

	SRF_RETURN_DONE(funcctx);
}

Datum
gg_duckdb_status(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[12];
	bool		nulls[12];
	const char *tempdir = gg_duckdb_instance_temp_directory();
	int			i = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	/*
	 * Report the effective settings: GUC changes are applied on the next
	 * connection, so make one if the instance already exists.
	 */
	if (gg_duckdb_instance_initialized())
	{
		duckdb_connection conn = gg_duckdb_connect();

		gg_duckdb_disconnect(&conn);
	}

	memset(nulls, 0, sizeof(nulls));
	values[i++] = BoolGetDatum(gg_duckdb_preloaded);
	values[i++] = PointerGetDatum(cstring_to_text(duckdb_library_version()));
	values[i++] = PointerGetDatum(cstring_to_text(GG_DUCKDB_PINNED_VERSION));
	values[i++] = BoolGetDatum(gg_duckdb_instance_initialized());
	values[i++] = Int32GetDatum(gg_duckdb_instance_threads());
	values[i++] = Int64GetDatum(gg_duckdb_instance_memory_limit_bytes());
	if (tempdir)
	{
		values[i++] = PointerGetDatum(cstring_to_text(tempdir));
		values[i++] = BoolGetDatum(access(tempdir, W_OK) == 0);
	}
	else
	{
		nulls[i++] = true;
		nulls[i++] = true;
	}
	values[i++] = Int64GetDatum(gg_duckdb_stats.queries_executed);
	values[i++] = Int64GetDatum(gg_duckdb_stats.errors);
	values[i++] = Int64GetDatum(gg_duckdb_stats.reopens);
	if (gg_duckdb_stats.last_error)
		values[i++] = PointerGetDatum(cstring_to_text(gg_duckdb_stats.last_error));
	else
		nulls[i++] = true;
	Assert(i == 12);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
