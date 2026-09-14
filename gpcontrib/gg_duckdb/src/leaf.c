/*-------------------------------------------------------------------------
 *
 * leaf.c
 *	  gg_leaf(i): the DuckDB table function through which a region pulls
 *	  the rows of its i-th leaf, an ordinary PostgreSQL plan node executed
 *	  by the standard executor.
 *
 * The function runs on the backend thread inside duckdb_pending_execute_task
 * (the instance has no threads of its own), so it may call ExecProcNode
 * directly.  Every batch is a PG_TRY barrier: a PostgreSQL error is copied
 * into the region, flushed, and reported to DuckDB as the function's error;
 * the region re-raises the original error when the DuckDB query fails.  A
 * longjmp never crosses DuckDB's frames, and no DuckDB call that can throw
 * (allocate) runs inside the barrier: see leaf_scan.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <alloca.h>
#include <stdlib.h>

#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/memutils.h"

#include "utils/faultinjector.h"

#include "gg_duckdb/gg_duckdb.h"

/*
 * What gg_leaf's bind sees while a region query is prepared: the leaf
 * descriptors, and the region when there is one (none while a query is
 * only validated on the coordinator).  Bind time is the only moment DuckDB
 * gives us no handle of our own.  Saved and restored around every prepare,
 * so that a region prepared from inside another region's leaf works.
 */
static GGBindContext *bind_ctx = NULL;

typedef struct LeafBindData
{
	GGRegionState *region;
	int			leaf;
} LeafBindData;

typedef struct LeafInitData
{
	int			ncols;			/* projected columns */
	int		   *col;			/* their 0-based positions in the leaf's slot */
} LeafInitData;

static void
leaf_free(void *p)
{
	free(p);
}

static void
leaf_init_free(void *p)
{
	LeafInitData *d = (LeafInitData *) p;

	free(d->col);
	free(d);
}

static void
leaf_bind(duckdb_bind_info info)
{
	GGBindContext *bctx = bind_ctx;
	duckdb_value pval;
	int64		leaf;
	LeafBindData *bd;
	GGLeafDesc *lf;
	int			i;

	if (bctx == NULL)
	{
		duckdb_bind_set_error(info, "gg_leaf() may only be used by a gg_duckdb region");
		return;
	}
	pval = duckdb_bind_get_parameter(info, 0);
	leaf = duckdb_get_int64(pval);
	duckdb_destroy_value(&pval);
	if (leaf < 0 || leaf >= bctx->nleaves)
	{
		duckdb_bind_set_error(info, "gg_leaf(): leaf index out of range");
		return;
	}
	lf = bctx->leaves[leaf];

	for (i = 0; i < lf->ncols; i++)
	{
		duckdb_logical_type lt = gg_duckdb_logical_type(&lf->cols[i].type);

		duckdb_bind_add_result_column(info, lf->cols[i].name, lt);
		duckdb_destroy_logical_type(&lt);
	}
	if (lf->ncols == 0)
	{
		/*
		 * A leaf that projects nothing (count(*) above it) still has rows.
		 * DuckDB requires a table function to return a column, so it gets a
		 * placeholder that nothing references; leaf_scan fills it with false.
		 */
		duckdb_logical_type lt = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);

		duckdb_bind_add_result_column(info, GG_DUCKDB_LEAF_PLACEHOLDER, lt);
		duckdb_destroy_logical_type(&lt);
	}
	duckdb_bind_set_cardinality(info, (idx_t) Max(lf->plan_rows, 1.0), false);

	bd = (LeafBindData *) malloc(sizeof(LeafBindData));
	bd->region = bctx->region;
	bd->leaf = (int) leaf;
	duckdb_bind_set_bind_data(info, bd, leaf_free);
}

static void
leaf_init(duckdb_init_info info)
{
	LeafBindData *bd = (LeafBindData *) duckdb_init_get_bind_data(info);
	LeafInitData *id = (LeafInitData *) malloc(sizeof(LeafInitData));
	idx_t		n = duckdb_init_get_column_count(info);
	idx_t		i;

	if (bd->region == NULL)
	{
		free(id);
		duckdb_init_set_error(info, "gg_leaf(): this query was only validated, it cannot run");
		return;
	}
	id->ncols = (int) n;
	id->col = (int *) malloc(sizeof(int) * Max(n, 1));
	for (i = 0; i < n; i++)
		id->col[i] = (int) duckdb_init_get_column_index(info, i);
	duckdb_init_set_init_data(info, id, leaf_init_free);
	duckdb_init_set_max_threads(info, 1);
}

/*
 * Pull up to one vector's worth of rows from the leaf into `output`.
 *
 * Two phases.  Inside the PG_TRY barrier the rows are pulled and converted:
 * fixed-width values are written straight into the vectors' memory, strings
 * are copied into the batch context and remembered.  Outside it the strings
 * are handed to DuckDB.  No DuckDB call that can allocate runs inside the
 * barrier: an allocation failure under the memory limit is a C++ exception,
 * which unwinds through this C frame to DuckDB's own handler; it must not
 * skip a PG_END_TRY, or the backend's exception stack would point into a
 * dead frame and the next ereport would jump into it.
 */
typedef struct StagedStrings
{
	int32	   *offset;			/* per row: into `buf`, or -1 for NULL */
	int32	   *len;
} StagedStrings;

static void
leaf_scan(duckdb_function_info info, duckdb_data_chunk output)
{
	LeafBindData *bd = (LeafBindData *) duckdb_function_get_bind_data(info);
	LeafInitData *id = (LeafInitData *) duckdb_function_get_init_data(info);
	GGRegionState *region = bd->region;
	GGLeaf	   *lf = &region->leaves[bd->leaf];
	idx_t		capacity = duckdb_vector_size();
	idx_t		n = 0;
	MemoryContext oldcxt;
	int			maxattno = 0;
	int			j;
	GGVectorTarget *targets;
	StagedStrings *staged;
	bool		any_strings = false;
	StringInfoData buf;

	if (lf->eof || region->q.pending_error != NULL)
	{
		duckdb_data_chunk_set_size(output, 0);
		return;
	}

	targets = (GGVectorTarget *) alloca(sizeof(GGVectorTarget) * Max(id->ncols, 1));
	staged = (StagedStrings *) alloca(sizeof(StagedStrings) * Max(id->ncols, 1));
	for (j = 0; j < id->ncols; j++)
	{
		duckdb_vector vec = duckdb_data_chunk_get_vector(output, j);
		int			col = id->col[j];

		staged[j].offset = NULL;
		staged[j].len = NULL;
		if (col < lf->desc.ncols)
		{
			const GGTypeInfo *ti = &lf->desc.cols[col].type;

			gg_duckdb_vector_target(vec, ti, &targets[j]);
			if (ti->duck == DUCKDB_TYPE_VARCHAR || ti->duck == DUCKDB_TYPE_BLOB)
				any_strings = true;
			if (col + 1 > maxattno)
				maxattno = col + 1;
		}
		else
		{
			/* the placeholder column of a leaf without columns */
			memset(&targets[j], 0, sizeof(targets[j]));
			targets[j].vec = vec;
			targets[j].data = duckdb_vector_get_data(vec);
		}
	}

	/*
	 * The leaf runs in whatever context the region was called in, exactly as
	 * if its parent had called it: what it allocates lazily (a scan
	 * descriptor, for one) must outlive this batch.  Only the conversion of
	 * its values runs in the batch context, which is reset every batch.
	 */
	oldcxt = CurrentMemoryContext;
	MemoryContextReset(region->q.batch_cxt);
	buf.data = NULL;

	PG_TRY();
	{
		CHECK_FOR_INTERRUPTS();
		SIMPLE_FAULT_INJECTOR("gg_duckdb_before_batch");

		if (any_strings)
		{
			MemoryContextSwitchTo(region->q.batch_cxt);
			initStringInfo(&buf);
			for (j = 0; j < id->ncols; j++)
			{
				int			col = id->col[j];

				if (col < lf->desc.ncols &&
					(lf->desc.cols[col].type.duck == DUCKDB_TYPE_VARCHAR ||
					 lf->desc.cols[col].type.duck == DUCKDB_TYPE_BLOB))
				{
					staged[j].offset = palloc(sizeof(int32) * capacity);
					staged[j].len = palloc(sizeof(int32) * capacity);
				}
			}
			MemoryContextSwitchTo(oldcxt);
		}

		while (n < capacity)
		{
			TupleTableSlot *slot = ExecProcNode(lf->ps);

			if (TupIsNull(slot))
			{
				lf->eof = true;
				break;
			}
			if (maxattno > 0)
				slot_getsomeattrs(slot, maxattno);
			MemoryContextSwitchTo(region->q.batch_cxt);
			for (j = 0; j < id->ncols; j++)
			{
				int			col = id->col[j];
				const char *str;
				int			len;

				if (col >= lf->desc.ncols)
				{
					((bool *) targets[j].data)[n] = false;
					continue;
				}
				gg_duckdb_write_datum(&lf->desc.cols[col].type, &targets[j], n,
									  slot->tts_values[col], slot->tts_isnull[col],
									  &str, &len);
				if (staged[j].offset != NULL)
				{
					/* the datum lives only until the next row: copy it */
					if (str == NULL)
					{
						staged[j].offset[n] = -1;
						staged[j].len[n] = -1;
					}
					else
					{
						staged[j].offset[n] = buf.len;
						staged[j].len[n] = len;
						appendBinaryStringInfo(&buf, str, len);
					}
				}
			}
			MemoryContextSwitchTo(oldcxt);
			n++;
		}
		lf->rows += n;
	}
	PG_CATCH();
	{
		MemoryContext ecxt;
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		ecxt = MemoryContextSwitchTo(region->q.query_cxt);
		edata = CopyErrorData();
		MemoryContextSwitchTo(ecxt);
		FlushErrorState();
		if (region->q.pending_error == NULL)
			region->q.pending_error = edata;
		duckdb_function_set_error(info, edata->message ? edata->message : "error in leaf");
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	PG_END_TRY();

	/* outside the barrier: the strings go into DuckDB's heap */
	for (j = 0; j < id->ncols; j++)
	{
		idx_t		r;

		if (staged[j].offset == NULL)
			continue;
		for (r = 0; r < n; r++)
		{
			if (staged[j].offset[r] >= 0)
				duckdb_vector_assign_string_element_len(targets[j].vec, r,
														buf.data + staged[j].offset[r],
														staged[j].len[r]);
		}
	}

	duckdb_data_chunk_set_size(output, n);
}

/*
 * Register gg_leaf on a connection of the instance (the catalog is
 * instance-wide, so once per instance).
 */
void
gg_duckdb_register_leaf_function(duckdb_connection conn)
{
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);

	duckdb_table_function_set_name(tf, GG_DUCKDB_LEAF_FUNCTION);
	duckdb_table_function_add_parameter(tf, bigint);
	duckdb_table_function_set_bind(tf, leaf_bind);
	duckdb_table_function_set_init(tf, leaf_init);
	duckdb_table_function_set_function(tf, leaf_scan);
	duckdb_table_function_supports_projection_pushdown(tf, true);
	if (duckdb_register_table_function(conn, tf) == DuckDBError)
	{
		duckdb_destroy_table_function(&tf);
		duckdb_destroy_logical_type(&bigint);
		elog(ERROR, "gg_duckdb: could not register the %s table function", GG_DUCKDB_LEAF_FUNCTION);
	}
	duckdb_destroy_table_function(&tf);
	duckdb_destroy_logical_type(&bigint);
}

/*
 * Describe a leaf from its Plan node (what the coordinator has at plan
 * time); the QE derives the same shape from the PlanState's result type.
 */
void
gg_duckdb_describe_leaf_plan(GGLeafDesc *desc, Plan *plan)
{
	ListCell   *lc;
	int			k = 0;

	desc->ncols = list_length(plan->targetlist);
	desc->cols = palloc0(sizeof(GGLeafCol) * Max(desc->ncols, 1));
	desc->plan_rows = plan->plan_rows;
	foreach(lc, plan->targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);

		snprintf(desc->cols[k].name, sizeof(desc->cols[k].name), "c%d", k + 1);
		if (!gg_duckdb_leaf_column_type(plan, k, &desc->cols[k].type))
			elog(ERROR, "gg_duckdb: leaf column %d has an unsupported type", k + 1);
		(void) te;
		k++;
	}
}

/*
 * Make `bctx` visible to gg_leaf's bind until the matching pop.  DuckDB binds
 * a statement with parameters when it is first executed, not when it is
 * prepared, so a region keeps its context pushed from prepare through the
 * start of execution.
 */
GGBindContext *
gg_duckdb_bind_context_push(GGBindContext *bctx)
{
	GGBindContext *saved = bind_ctx;

	bind_ctx = bctx;
	return saved;
}

void
gg_duckdb_bind_context_pop(GGBindContext *saved)
{
	bind_ctx = saved;
}

/* Prepare `sql` on `conn` with the bind context visible to gg_leaf's bind. */
duckdb_prepared_statement
gg_duckdb_prepare_with(GGBindContext *bctx, duckdb_connection conn,
					   const char *sql, char **errmsg)
{
	GGBindContext *saved = bind_ctx;
	duckdb_prepared_statement stmt = NULL;
	duckdb_state rc;

	bind_ctx = bctx;
	rc = duckdb_prepare(conn, sql, &stmt);
	bind_ctx = saved;

	if (rc == DuckDBError)
	{
		const char *msg = duckdb_prepare_error(stmt);

		*errmsg = pstrdup(msg ? msg : "could not prepare the region query");
		duckdb_destroy_prepare(&stmt);
		return NULL;
	}
	*errmsg = NULL;
	return stmt;
}
