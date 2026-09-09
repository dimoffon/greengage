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
 * longjmp never crosses DuckDB's frames.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <alloca.h>
#include <stdlib.h>

#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/memutils.h"

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
 */
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
	duckdb_vector *vecs;
	void	  **data;
	uint64_t  **validity;

	if (lf->eof || region->pending_error != NULL)
	{
		duckdb_data_chunk_set_size(output, 0);
		return;
	}

	vecs = (duckdb_vector *) alloca(sizeof(duckdb_vector) * Max(id->ncols, 1));
	data = (void **) alloca(sizeof(void *) * Max(id->ncols, 1));
	validity = (uint64_t **) alloca(sizeof(uint64_t *) * Max(id->ncols, 1));
	for (j = 0; j < id->ncols; j++)
	{
		vecs[j] = duckdb_data_chunk_get_vector(output, j);
		data[j] = duckdb_vector_get_data(vecs[j]);
		duckdb_vector_ensure_validity_writable(vecs[j]);
		validity[j] = duckdb_vector_get_validity(vecs[j]);
		if (id->col[j] + 1 > maxattno)
			maxattno = id->col[j] + 1;
	}

	/*
	 * The leaf runs in whatever context the region was called in, exactly as
	 * if its parent had called it: what it allocates lazily (a scan
	 * descriptor, for one) must outlive this batch.  Only the conversion of
	 * its values runs in the batch context, which is reset every batch.
	 */
	oldcxt = CurrentMemoryContext;
	MemoryContextReset(region->batch_cxt);

	PG_TRY();
	{
		CHECK_FOR_INTERRUPTS();

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
			MemoryContextSwitchTo(region->batch_cxt);
			for (j = 0; j < id->ncols; j++)
			{
				int			col = id->col[j];

				gg_duckdb_write_datum(&lf->desc.cols[col].type,
									  vecs[j], data[j], validity[j], n,
									  slot->tts_values[col], slot->tts_isnull[col]);
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
		ecxt = MemoryContextSwitchTo(region->region_cxt);
		edata = CopyErrorData();
		MemoryContextSwitchTo(ecxt);
		FlushErrorState();
		if (region->pending_error == NULL)
			region->pending_error = edata;
		duckdb_function_set_error(info, edata->message ? edata->message : "error in leaf");
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	PG_END_TRY();

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
		if (!gg_duckdb_type_map(exprType((Node *) te->expr), exprTypmod((Node *) te->expr),
								&desc->cols[k].type))
			elog(ERROR, "gg_duckdb: leaf column %d has an unsupported type", k + 1);
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
