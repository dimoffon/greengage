/*-------------------------------------------------------------------------
 *
 * leaf.c
 *	  The DuckDB table functions through which a region pulls its leaves.
 *	  gg_leaf(i) runs the i-th leaf, an ordinary PostgreSQL plan node, with
 *	  the standard executor.  gg_rel(i) reads the heap table under the i-th
 *	  leaf, a sequential scan whose target list and filter the region's query
 *	  computes itself, page by page and without the scan node.
 *
 * Both run on the backend thread inside duckdb_pending_execute_task (the
 * instance has no threads of its own), so they may call into PostgreSQL
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

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/tupmacs.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "utils/faultinjector.h"

#include "gg_duckdb/gg_duckdb.h"

/*
 * What the functions' bind sees while a region query is prepared: the leaf
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
	bool		rel;			/* gg_rel: the leaf's table, read directly */
} LeafBindData;

typedef struct LeafInitData
{
	int			ncols;			/* projected columns */
	int		   *col;			/* their 0-based positions among the function's columns */
	int			maxatt;			/* gg_rel: attributes to deform */
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

/*
 * gg_leaf returns the leaf's result columns c1..cN; gg_rel the attributes of
 * the table the region's query uses, named c<attribute number>.
 */
static void
leaf_bind_common(duckdb_bind_info info, bool rel)
{
	GGBindContext *bctx = bind_ctx;
	duckdb_value pval;
	int64		leaf;
	LeafBindData *bd;
	GGLeafDesc *lf;
	const GGLeafCol *cols;
	int			ncols;
	int			i;

	if (bctx == NULL)
	{
		duckdb_bind_set_error(info, rel ? "gg_rel() may only be used by a gg_duckdb region"
							  : "gg_leaf() may only be used by a gg_duckdb region");
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
	if (rel && lf->nrel < 0)
	{
		duckdb_bind_set_error(info, "gg_rel(): the leaf is not read directly");
		return;
	}
	cols = rel ? lf->rel_cols : lf->cols;
	ncols = rel ? lf->nrel : lf->ncols;

	for (i = 0; i < ncols; i++)
	{
		duckdb_logical_type lt = gg_duckdb_logical_type(&cols[i].type);

		duckdb_bind_add_result_column(info, cols[i].name, lt);
		duckdb_destroy_logical_type(&lt);
	}
	if (ncols == 0)
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
	bd->rel = rel;
	duckdb_bind_set_bind_data(info, bd, leaf_free);
}

static void
leaf_bind(duckdb_bind_info info)
{
	leaf_bind_common(info, false);
}

static void
rel_bind(duckdb_bind_info info)
{
	leaf_bind_common(info, true);
}

static void
leaf_init(duckdb_init_info info)
{
	LeafBindData *bd = (LeafBindData *) duckdb_init_get_bind_data(info);
	LeafInitData *id;
	idx_t		n = duckdb_init_get_column_count(info);
	idx_t		i;

	if (bd->region == NULL)
	{
		duckdb_init_set_error(info, "gg_leaf(): this query was only validated, it cannot run");
		return;
	}
	id = (LeafInitData *) malloc(sizeof(LeafInitData));
	id->ncols = (int) n;
	id->col = (int *) malloc(sizeof(int) * Max(n, 1));
	id->maxatt = 0;
	for (i = 0; i < n; i++)
		id->col[i] = (int) duckdb_init_get_column_index(info, i);
	if (bd->rel)
	{
		const GGLeafDesc *d = &bd->region->leaves[bd->leaf].desc;

		/* projection pushdown: only the attributes DuckDB asks for are deformed */
		for (i = 0; i < n; i++)
			if (id->col[i] < d->nrel)
				id->maxatt = Max(id->maxatt, d->rel_att[id->col[i]] + 1);
	}
	duckdb_init_set_init_data(info, id, leaf_init_free);
	duckdb_init_set_max_threads(info, 1);
}

typedef struct StagedStrings
{
	int32	   *offset;			/* per row: into `buf`, or -1 for NULL */
	int32	   *len;
} StagedStrings;

/*
 * Write one row's projected columns at position `n` of the vectors:
 * `values`/`isnull` are indexed through `map` (function column -> attribute)
 * when given, else by the function column.  Runs in the batch context;
 * strings are copied there, to be handed to DuckDB after the barrier.
 */
static inline void
leaf_put_row(const GGLeafCol *cols, int ncols, LeafInitData *id,
			 GGVectorTarget *targets, StagedStrings *staged, StringInfo buf,
			 idx_t n, Datum *values, bool *isnull, const int *map)
{
	int			j;

	for (j = 0; j < id->ncols; j++)
	{
		int			col = id->col[j];
		int			a;
		const char *str;
		int			len;

		if (col >= ncols)
		{
			((bool *) targets[j].data)[n] = false;
			continue;
		}
		a = map ? map[col] : col;
		gg_duckdb_write_datum(&cols[col].type, &targets[j], n, values[a], isnull[a], &str, &len);
		if (staged[j].offset != NULL)
		{
			/* the datum lives only until the next row or page: copy it */
			if (str == NULL)
			{
				staged[j].offset[n] = -1;
				staged[j].len[n] = -1;
			}
			else
			{
				staged[j].offset[n] = buf->len;
				staged[j].len[n] = len;
				appendBinaryStringInfo(buf, str, len);
			}
		}
	}
}

/* ---------- gg_rel: the heap table under a sequential scan, read directly ---------- */

/*
 * The heap scan of a direct leaf: over the relation its sequential scan
 * opened and locked, under the executor's snapshot, so that it returns the
 * rows the scan would have looked at.  The region's query applies the scan's
 * filter.
 */
static void
leaf_rel_begin(GGRegionState *region, GGLeaf *lf)
{
	Relation	rel = ((ScanState *) lf->ps)->ss_currentRelation;
	int			natts = RelationGetDescr(rel)->natts;
	MemoryContext oldcxt = MemoryContextSwitchTo(region->q.query_cxt);

	if (lf->rel_values == NULL)
	{
		lf->rel_values = palloc(sizeof(Datum) * Max(natts, 1));
		lf->rel_isnull = palloc(sizeof(bool) * Max(natts, 1));
	}
	lf->rel_scan = table_beginscan(rel, lf->ps->state->es_snapshot, 0, NULL);
	MemoryContextSwitchTo(oldcxt);
}

void
gg_duckdb_leaf_rel_close(GGLeaf *lf)
{
	if (lf->rel_scan != NULL)
	{
		table_endscan(lf->rel_scan);
		lf->rel_scan = NULL;
	}
}

/* The first `natts` attributes of a heap tuple: slot_deform_heap_tuple without the slot. */
static inline void
leaf_rel_deform(TupleDesc tdesc, HeapTupleHeader tup, int natts, Datum *values, bool *isnull)
{
	bool		hasnulls = (tup->t_infomask & HEAP_HASNULL) != 0;
	bits8	   *bp = tup->t_bits;
	char	   *tp = (char *) tup + tup->t_hoff;
	int			tupnatts = HeapTupleHeaderGetNatts(tup);
	uint32		off = 0;
	bool		slow = false;
	int			attnum;

	for (attnum = 0; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = TupleDescAttr(tdesc, attnum);

		if (attnum >= tupnatts)
		{
			/* added after the tuple was written: its default, or NULL */
			values[attnum] = getmissingattr(tdesc, attnum + 1, &isnull[attnum]);
			continue;
		}
		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			slow = true;
			continue;
		}
		isnull[attnum] = false;

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else if (thisatt->attlen == -1)
		{
			if (!slow && off == att_align_nominal(off, thisatt->attalign))
				thisatt->attcacheoff = off;
			else
			{
				off = att_align_pointer(off, thisatt->attalign, -1, tp + off);
				slow = true;
			}
		}
		else
		{
			off = att_align_nominal(off, thisatt->attalign);
			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);
		off = att_addlength_pointer(off, thisatt->attlen, tp + off);
		if (thisatt->attlen <= 0)
			slow = true;
	}
}

/*
 * Up to `capacity` rows of a direct leaf.  With an MVCC snapshot (what a
 * query has) the scan works a page at a time: heapgetpage prunes the page
 * and lists its visible tuples, which are deformed in place while the page
 * stays pinned.  Otherwise the scan fills the sequential scan's own slot.
 */
static idx_t
leaf_rel_rows(GGRegionState *region, GGLeaf *lf, LeafInitData *id,
			  const GGLeafCol *cols, int ncols, GGVectorTarget *targets,
			  StagedStrings *staged, StringInfo buf, idx_t capacity, MemoryContext oldcxt)
{
	idx_t		n = 0;

	if (lf->rel_scan == NULL)
		leaf_rel_begin(region, lf);

	if (lf->rel_scan->rs_flags & SO_ALLOW_PAGEMODE)
	{
		HeapScanDesc hs = (HeapScanDesc) lf->rel_scan;
		TupleDesc	tdesc = RelationGetDescr(lf->rel_scan->rs_rd);

		while (n < capacity)
		{
			Page		page;
			HeapTupleHeader tup;

			if (!hs->rs_inited || hs->rs_cindex >= hs->rs_ntuples)
			{
				BlockNumber blk = hs->rs_inited ? hs->rs_cblock + 1 : 0;

				if (blk >= hs->rs_nblocks)
				{
					lf->eof = true;
					break;
				}
				heapgetpage(lf->rel_scan, blk); /* checks for interrupts */
				hs->rs_inited = true;
				hs->rs_cindex = 0;
				continue;
			}
			page = BufferGetPage(hs->rs_cbuf);
			tup = (HeapTupleHeader) PageGetItem(page,
												PageGetItemId(page, hs->rs_vistuples[hs->rs_cindex]));
			hs->rs_cindex++;
			leaf_rel_deform(tdesc, tup, id->maxatt, lf->rel_values, lf->rel_isnull);
			MemoryContextSwitchTo(region->q.batch_cxt);
			leaf_put_row(cols, ncols, id, targets, staged, buf, n,
						 lf->rel_values, lf->rel_isnull, lf->desc.rel_att);
			MemoryContextSwitchTo(oldcxt);
			n++;
		}
	}
	else
	{
		TupleTableSlot *slot = ((ScanState *) lf->ps)->ss_ScanTupleSlot;

		while (n < capacity)
		{
			if (!table_scan_getnextslot(lf->rel_scan, ForwardScanDirection, slot))
			{
				lf->eof = true;
				break;
			}
			if (id->maxatt > 0)
				slot_getsomeattrs(slot, id->maxatt);
			MemoryContextSwitchTo(region->q.batch_cxt);
			leaf_put_row(cols, ncols, id, targets, staged, buf, n,
						 slot->tts_values, slot->tts_isnull, lf->desc.rel_att);
			MemoryContextSwitchTo(oldcxt);
			n++;
			CHECK_FOR_INTERRUPTS();
		}
	}
	return n;
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
static void
leaf_scan(duckdb_function_info info, duckdb_data_chunk output)
{
	LeafBindData *bd = (LeafBindData *) duckdb_function_get_bind_data(info);
	LeafInitData *id = (LeafInitData *) duckdb_function_get_init_data(info);
	GGRegionState *region = bd->region;
	GGLeaf	   *lf = &region->leaves[bd->leaf];
	const GGLeafCol *cols = bd->rel ? lf->desc.rel_cols : lf->desc.cols;
	int			ncols = bd->rel ? lf->desc.nrel : lf->desc.ncols;
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
		if (col < ncols)
		{
			const GGTypeInfo *ti = &cols[col].type;

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

				if (col < ncols &&
					(cols[col].type.duck == DUCKDB_TYPE_VARCHAR ||
					 cols[col].type.duck == DUCKDB_TYPE_BLOB))
				{
					staged[j].offset = palloc(sizeof(int32) * capacity);
					staged[j].len = palloc(sizeof(int32) * capacity);
				}
			}
			MemoryContextSwitchTo(oldcxt);
		}

		if (bd->rel)
			n = leaf_rel_rows(region, lf, id, cols, ncols, targets, staged, &buf,
							  capacity, oldcxt);
		else
		{
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
				leaf_put_row(cols, ncols, id, targets, staged, &buf, n,
							 slot->tts_values, slot->tts_isnull, NULL);
				MemoryContextSwitchTo(oldcxt);
				n++;
			}
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

static void
register_function(duckdb_connection conn, const char *name, duckdb_table_function_bind_t bind)
{
	duckdb_table_function tf = duckdb_create_table_function();
	duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);

	duckdb_table_function_set_name(tf, name);
	duckdb_table_function_add_parameter(tf, bigint);
	duckdb_table_function_set_bind(tf, bind);
	duckdb_table_function_set_init(tf, leaf_init);
	duckdb_table_function_set_function(tf, leaf_scan);
	duckdb_table_function_supports_projection_pushdown(tf, true);
	if (duckdb_register_table_function(conn, tf) == DuckDBError)
	{
		duckdb_destroy_table_function(&tf);
		duckdb_destroy_logical_type(&bigint);
		elog(ERROR, "gg_duckdb: could not register the %s table function", name);
	}
	duckdb_destroy_table_function(&tf);
	duckdb_destroy_logical_type(&bigint);
}

/*
 * Register gg_leaf and gg_rel on a connection of the instance (the catalog
 * is instance-wide, so once per instance).
 */
void
gg_duckdb_register_leaf_function(duckdb_connection conn)
{
	register_function(conn, GG_DUCKDB_LEAF_FUNCTION, leaf_bind);
	register_function(conn, GG_DUCKDB_REL_FUNCTION, rel_bind);
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
	desc->nrel = -1;
	desc->rel_att = NULL;
	desc->rel_cols = NULL;
	foreach(lc, plan->targetlist)
	{
		snprintf(desc->cols[k].name, sizeof(desc->cols[k].name), "c%d", k + 1);
		if (!gg_duckdb_leaf_column_type(plan, k, &desc->cols[k].type))
			elog(ERROR, "gg_duckdb: leaf column %d has an unsupported type", k + 1);
		k++;
	}
}

/* The gg_rel columns of a direct leaf, as the coordinator's deparser chose them. */
void
gg_duckdb_describe_rel_leaf(GGLeafDesc *desc, const GGRelScan *rs)
{
	int			k;

	desc->nrel = rs->ncols;
	desc->rel_att = palloc(sizeof(int) * Max(rs->ncols, 1));
	desc->rel_cols = palloc0(sizeof(GGLeafCol) * Max(rs->ncols, 1));
	for (k = 0; k < rs->ncols; k++)
	{
		desc->rel_att[k] = rs->attnos[k] - 1;
		snprintf(desc->rel_cols[k].name, sizeof(desc->rel_cols[k].name), "c%d", (int) rs->attnos[k]);
		desc->rel_cols[k].type = rs->types[k];
	}
}

/*
 * Make `bctx` visible to the functions' bind until the matching pop.  DuckDB
 * binds a statement with parameters when it is first executed, not when it
 * is prepared, so a region keeps its context pushed from prepare through the
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

/* Prepare `sql` on `conn` with the bind context visible to the functions' bind. */
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
