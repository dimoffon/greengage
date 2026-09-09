/*-------------------------------------------------------------------------
 *
 * node.c
 *	  The DuckDB region: a CustomScan whose interior runs in the embedded
 *	  DuckDB and whose leaves are ordinary plan nodes kept in custom_ps, or
 *	  gg_duckdb foreign tables DuckDB reads natively.
 *
 * The query itself (prepare, bind, execute, stream, convert) is the
 * shared executor of query.c; this file owns what is specific to a region:
 * its private data, its leaves and their bind context, and its EXPLAIN.
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
#include "utils/lsyscache.h"
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

/*
 * Layout (a List of Const): version, sql, flags, nleaves, label, nparams,
 * params..., nout, output descriptors..., then per leaf its column count
 * and column descriptors, then the native leaves (count, then per native
 * leaf its relation OID, reader call and empty relation).  The leaf shapes
 * are what the coordinator's deparser assumed; a segment cannot re-derive
 * them once the subtree below a leaf has itself become a region.
 */
List *
gg_duckdb_make_private(const char *sql, int flags, List *leaves, const char *label,
					   List *params, int nout, const GGTypeInfo *outtypes, List *natives)
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
	priv = lappend(priv, make_int_const(list_length(natives)));
	foreach(lc, natives)
	{
		GGNativeLeaf *nl = (GGNativeLeaf *) lfirst(lc);

		priv = lappend(priv, make_int_const((int) nl->relid));
		priv = lappend(priv, make_text_const(nl->reader));
		priv = lappend(priv, make_text_const(nl->empty));
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
	int			nparams,
				nout,
				nnatives,
				pos,
				i;

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

	nparams = private_int(priv, GGP_NPARAMS);
	pos = GGP_NPARAMS + 1;
	if (list_length(priv) < pos + nparams + 1)
		elog(ERROR, "gg_duckdb: malformed region node");
	for (i = 0; i < nparams; i++)
		st->q.params = lappend(st->q.params, list_nth(priv, pos + i));
	pos += nparams;
	nout = private_int(priv, pos);
	pos++;
	if (list_length(priv) < pos + nout)
		elog(ERROR, "gg_duckdb: malformed region node");
	st->q.ncols = nout;
	st->q.outtypes = palloc0(sizeof(GGTypeInfo) * Max(nout, 1));
	for (i = 0; i < nout; i++)
	{
		int			d = private_int(priv, pos + i);

		st->q.outtypes[i].duck = (duckdb_type) (d >> 16);
		st->q.outtypes[i].width = (uint8) ((d >> 8) & 0xff);
		st->q.outtypes[i].scale = (uint8) (d & 0xff);
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
	if (list_length(priv) < pos + 1)
		elog(ERROR, "gg_duckdb: malformed region node");
	nnatives = private_int(priv, pos);
	pos++;
	if (list_length(priv) < pos + 3 * nnatives)
		elog(ERROR, "gg_duckdb: malformed region node");
	for (i = 0; i < nnatives; i++)
	{
		GGNativeLeaf *nl = palloc0(sizeof(GGNativeLeaf));

		nl->relid = (Oid) private_int(priv, pos);
		nl->reader = private_text(priv, pos + 1);
		nl->empty = private_text(priv, pos + 2);
		st->natives = lappend(st->natives, nl);
		pos += 3;
	}

	return (Node *) st;
}

static void
describe_leaf(GGRegionState *st, GGLeaf *lf, PlanState *ps, int leafno)
{
	TupleDesc	desc = ExecGetResultType(ps);
	int			k;

	lf->ps = ps;
	lf->desc.ncols = desc->natts;
	lf->desc.cols = MemoryContextAllocZero(st->q.query_cxt, sizeof(GGLeafCol) * Max(desc->natts, 1));
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
	ListCell   *lc;
	int			i;

	gg_duckdb_query_init(&st->q, &node->ss.ps, estate->es_query_cxt, "region");

	if (list_length(cscan->custom_plans) != st->nleaves)
		elog(ERROR, "gg_duckdb: region declares %d leaves but carries %d",
			 st->nleaves, list_length(cscan->custom_plans));
	st->leaves = MemoryContextAllocZero(st->q.query_cxt, sizeof(GGLeaf) * Max(st->nleaves, 1));

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

	{
		TupleDesc	desc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
		int		   *colmap = NULL;

		if (desc->natts == 0 && st->q.ncols == 1)
		{
			/* a region without columns: the placeholder column is discarded */
			colmap = palloc(sizeof(int));
			colmap[0] = -1;
		}
		gg_duckdb_query_set_output(&st->q, desc, st->q.outtypes, st->q.ncols, colmap);
	}
}

/*
 * The region's SQL with every native token replaced: by the reader over the
 * files of that leaf, bound as a list parameter after the constants, or by
 * the empty relation when the list is empty.  With `resolve` false every
 * token becomes the empty relation, which is what plan-time validation
 * prepares: the same shape, no files.
 */
char *
gg_duckdb_region_sql(const char *sql, List *natives, int nconst_params, bool resolve,
					 List **file_lists)
{
	StringInfoData out;
	int			next_param = nconst_params + 1;
	ListCell   *lc;
	int			i = 0;

	if (file_lists)
		*file_lists = NIL;
	if (natives == NIL)
		return pstrdup(sql);

	initStringInfo(&out);
	appendStringInfoString(&out, sql);
	foreach(lc, natives)
	{
		GGNativeLeaf *nl = (GGNativeLeaf *) lfirst(lc);
		char	   *token = psprintf(GG_NATIVE_TOKEN_FMT, i);
		char	   *at = strstr(out.data, token);
		List	   *files = resolve ? gg_duckdb_native_files(nl->relid) : NIL;
		char	   *replacement;
		StringInfoData rebuilt;

		if (at == NULL)
			elog(ERROR, "gg_duckdb: native leaf %d is not referenced by the region query", i);
		if (files == NIL)
			replacement = nl->empty;
		else
		{
			replacement = psprintf(nl->reader, psprintf("$%d", next_param++));
			if (file_lists)
				*file_lists = lappend(*file_lists, files);
		}
		initStringInfo(&rebuilt);
		appendBinaryStringInfo(&rebuilt, out.data, at - out.data);
		appendStringInfoString(&rebuilt, replacement);
		appendStringInfoString(&rebuilt, at + strlen(token));
		pfree(out.data);
		out = rebuilt;
		i++;
	}
	return out.data;
}

static TupleTableSlot *
region_exec(CustomScanState *node)
{
	GGRegionState *st = (GGRegionState *) node;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	if (st->q.finished)
		return ExecClearTuple(slot);
	if (!st->q.started)
	{
		GGBindContext bctx;
		GGLeafDesc **descs = palloc(sizeof(GGLeafDesc *) * Max(st->nleaves, 1));
		int			i;

		for (i = 0; i < st->nleaves; i++)
			descs[i] = &st->leaves[i].desc;
		bctx.region = st;
		bctx.nleaves = st->nleaves;
		bctx.leaves = descs;
		st->q.sql = gg_duckdb_region_sql(st->sql, st->natives, list_length(st->q.params),
										 true, &st->q.file_lists);
		st->q.pre_sql = gg_duckdb_native_pre_sql(st->natives);
		gg_duckdb_query_start(&st->q, &bctx);
	}
	gg_duckdb_query_next(&st->q, slot);
	return slot;
}

static void
region_end(CustomScanState *node)
{
	GGRegionState *st = (GGRegionState *) node;
	ListCell   *lc;

	gg_duckdb_query_release(&st->q);
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

	gg_duckdb_query_reset(&st->q);

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

	gg_duckdb_query_release_query(&st->q);
	st->q.finished = true;
}

/* EXPLAIN ANALYZE text from the QE that ran the region. */
static void
region_explain_end(PlanState *planstate, struct StringInfoData *buf)
{
	GGRegionState *st = (GGRegionState *) planstate;
	int			i;

	if (!st->q.started && st->q.rows_out == 0)
		return;
	gg_duckdb_query_explain_end(&st->q, buf);
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
	if (st->natives != NIL)
	{
		StringInfoData buf;
		ListCell   *lc;

		initStringInfo(&buf);
		foreach(lc, st->natives)
		{
			GGNativeLeaf *nl = (GGNativeLeaf *) lfirst(lc);
			char	   *name = get_rel_name(nl->relid);

			appendStringInfo(&buf, "%s%s (%s)", buf.len > 0 ? ", " : "",
							 name ? name : "?", gg_duckdb_native_location_text(nl->relid));
		}
		ExplainPropertyText("DuckDB readers", buf.data, es);
	}
	if (es->verbose)
		ExplainPropertyText("DuckDB SQL", st->sql, es);
}
