/*-------------------------------------------------------------------------
 *
 * pass.c
 *	  The planner pass: runs on the finished PlannedStmt of both optimizers
 *	  (post_planner_hook) and replaces eligible subtrees with DuckDB regions.
 *
 * The pass walks the plan top-down, tracking the slice each node belongs
 * to.  At every node of a segment slice it asks the deparser for the
 * maximal region rooted there; the first node that yields an eligible,
 * gated region is replaced, and the walk continues below the region's
 * Motion leaves only (a Motion's sender side is another slice; regions
 * nested in one process are left for a later milestone).
 *
 * Every candidate the pass declines is reported as a NOTICE under
 * gg_duckdb.explain_decisions, with the deparser's reason.
 *
 * gg_duckdb.debug_wrap = scans (milestone M1) instead wraps every SeqScan of
 * a segment slice into an identity region, which exercises the executor
 * without any deparsing.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "cdb/cdbllize.h"
#include "cdb/cdbplan.h"
#include "cdb/cdbvars.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "optimizer/walkers.h"
#include "utils/builtins.h"

#include "gg_duckdb/gg_duckdb.h"

static post_planner_hook_type prev_post_planner_hook = NULL;

typedef struct PassContext
{
	plan_tree_base_prefix base;	/* required by the plan tree walkers */
	PlannedStmt *stmt;
	int			slice;			/* slice of the node being visited */
	Plan	   *parent;			/* parent of the node being visited */
	int			next_plan_node_id;
	int			wrapped;
} PassContext;

static Node *pass_mutator(Node *node, PassContext *ctx);

static bool
max_plan_node_id_walker(Node *node, PassContext *ctx)
{
	if (node == NULL)
		return false;
	if (is_plan_node(node))
	{
		int			id = ((Plan *) node)->plan_node_id;

		if (id >= ctx->next_plan_node_id)
			ctx->next_plan_node_id = id + 1;
	}
	return plan_tree_walker(node, max_plan_node_id_walker, ctx, true);
}

static bool
slice_runs_on_segments(PassContext *ctx)
{
	PlanSlice  *slice;

	if (ctx->stmt->numSlices <= 0 || ctx->slice < 0 || ctx->slice >= ctx->stmt->numSlices)
		return false;
	slice = &ctx->stmt->slices[ctx->slice];
	if (slice->gangType == GANGTYPE_PRIMARY_READER ||
		slice->gangType == GANGTYPE_PRIMARY_WRITER ||
		slice->gangType == GANGTYPE_SINGLETON_READER)
		return true;
	return gg_duckdb_on_coordinator;
}

static const char *
plan_node_name(Plan *plan)
{
	switch (nodeTag(plan))
	{
		case T_Result: return "Result";
		case T_Agg: return "Agg";
		case T_Sort: return "Sort";
		case T_Limit: return "Limit";
		case T_Unique: return "Unique";
		case T_Material: return "Material";
		case T_SeqScan: return "SeqScan";
		case T_IndexScan: return "IndexScan";
		case T_HashJoin: return "HashJoin";
		case T_NestLoop: return "NestLoop";
		case T_MergeJoin: return "MergeJoin";
		case T_Append: return "Append";
		case T_Motion: return "Motion";
		case T_ShareInputScan: return "ShareInputScan";
		case T_SubqueryScan: return "SubqueryScan";
		case T_WindowAgg: return "WindowAgg";
		default: return "plan node";
	}
}

static void
decision(PassContext *ctx, Plan *plan, const char *fmt,...) pg_attribute_printf(3, 4);

static void
decision(PassContext *ctx, Plan *plan, const char *fmt,...)
{
	va_list		ap;
	char		buf[1024];

	if (!gg_duckdb_explain_decisions)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ereport(NOTICE,
			(errmsg("gg_duckdb: %s (plan node %d): %s",
					plan_node_name(plan), plan->plan_node_id, buf)));
}

/* Parents that consume their input in order. */
static bool
parent_needs_order(Plan *parent)
{
	if (parent == NULL)
		return false;
	switch (nodeTag(parent))
	{
		case T_MergeJoin:
		case T_Unique:
		case T_WindowAgg:
			return true;
		case T_Agg:
			return ((Agg *) parent)->aggstrategy == AGG_SORTED;
		case T_Motion:
			return ((Motion *) parent)->sendSorted;
		default:
			return false;
	}
}

/*
 * Build the CustomScan that replaces `root` with region `spec`.
 */
static Plan *
make_region(PassContext *ctx, Plan *root, GGRegionSpec *spec)
{
	List	   *tlist = root->targetlist;
	List	   *colnames = NIL;
	List	   *scan_tlist = NIL;
	List	   *out_tlist = NIL;
	RangeTblEntry *rte;
	CustomScan *cs;
	ListCell   *lc;
	int			rteidx;
	int			k;

	/*
	 * A named-tuplestore entry, as pg_duckdb uses: the deparser expands it
	 * through its column lists, so EXPLAIN VERBOSE can name the columns.
	 * Nothing ever scans it.
	 */
	rteidx = list_length(ctx->stmt->rtable) + 1;
	k = 1;
	foreach(lc, tlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		Node	   *expr = (Node *) te->expr;
		char	   *name = te->resname ? pstrdup(te->resname) : psprintf("c%d", k);
		Var		   *scanvar = makeVar(rteidx, k, exprType(expr), exprTypmod(expr),
									  exprCollation(expr), 0);
		Var		   *outvar = makeVar(INDEX_VAR, k, exprType(expr), exprTypmod(expr),
									 exprCollation(expr), 0);

		colnames = lappend(colnames, makeString(name));
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) scanvar, k, name, te->resjunk));
		out_tlist = lappend(out_tlist, makeTargetEntry((Expr *) outvar, k, name, te->resjunk));
		k++;
	}

	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_NAMEDTUPLESTORE;
	rte->enrname = "gg_duckdb_region";
	rte->enrtuples = root->plan_rows;
	rte->eref = makeAlias("gg_duckdb_region", colnames);
	foreach(lc, tlist)
	{
		Node	   *expr = (Node *) ((TargetEntry *) lfirst(lc))->expr;

		rte->coltypes = lappend_oid(rte->coltypes, exprType(expr));
		rte->coltypmods = lappend_int(rte->coltypmods, exprTypmod(expr));
		rte->colcollations = lappend_oid(rte->colcollations, exprCollation(expr));
	}
	rte->inFromCl = false;
	rte->lateral = false;
	rte->inh = false;
	rte->requiredPerms = 0;
	ctx->stmt->rtable = lappend(ctx->stmt->rtable, rte);

	cs = makeNode(CustomScan);
	cs->flags = spec->flags;
	cs->custom_plans = spec->leaves;
	cs->custom_exprs = NIL;
	cs->custom_private = gg_duckdb_make_private(spec->sql, spec->flags, spec->nleaves,
												spec->label, spec->params,
												spec->ncols, spec->outtypes);
	cs->custom_scan_tlist = scan_tlist;
	cs->custom_relids = bms_make_singleton(rteidx);
	cs->methods = &gg_duckdb_scan_methods;
	cs->scan.scanrelid = 0;
	cs->scan.plan.targetlist = out_tlist;
	cs->scan.plan.qual = NIL;
	cs->scan.plan.lefttree = NULL;
	cs->scan.plan.righttree = NULL;
	cs->scan.plan.initPlan = NIL;
	cs->scan.plan.startup_cost = root->startup_cost;
	cs->scan.plan.total_cost = root->total_cost;
	cs->scan.plan.plan_rows = root->plan_rows;
	cs->scan.plan.plan_width = root->plan_width;
	cs->scan.plan.plan_node_id = ctx->next_plan_node_id++;
	cs->scan.plan.extParam = bms_copy(root->extParam);
	cs->scan.plan.allParam = bms_copy(root->allParam);
	cs->scan.plan.flow = root->flow;

	ctx->wrapped++;
	return (Plan *) cs;
}

/*
 * Prepare the region's query on this backend's DuckDB, so that a query
 * DuckDB cannot bind never leaves the planner.  Any failure, including a
 * DuckDB that cannot be opened, only means "no region here".
 */
static bool
validate_region(GGRegionSpec *spec, char **why)
{
	GGBindContext bctx;
	GGLeafDesc **descs;
	duckdb_connection conn = NULL;
	duckdb_prepared_statement stmt = NULL;
	char	   *err = NULL;
	ListCell   *lc;
	int			i = 0;
	bool		ok = true;
	MemoryContext oldcxt = CurrentMemoryContext;

	descs = palloc(sizeof(GGLeafDesc *) * Max(spec->nleaves, 1));
	foreach(lc, spec->leaves)
	{
		descs[i] = palloc0(sizeof(GGLeafDesc));
		gg_duckdb_describe_leaf_plan(descs[i], (Plan *) lfirst(lc));
		i++;
	}
	bctx.region = NULL;
	bctx.nleaves = spec->nleaves;
	bctx.leaves = descs;

	PG_TRY();
	{
		conn = gg_duckdb_connect();
		stmt = gg_duckdb_prepare_with(&bctx, conn, spec->sql, &err);
		if (stmt == NULL)
		{
			*why = psprintf("DuckDB could not prepare the query: %s", err);
			ok = false;
		}
		else
		{
			idx_t		n = duckdb_prepared_statement_column_count(stmt);

			if ((int) n != spec->ncols)
			{
				*why = psprintf("DuckDB returns %d columns, %d expected", (int) n, spec->ncols);
				ok = false;
			}
			for (i = 0; ok && i < spec->ncols; i++)
			{
				duckdb_type t = duckdb_prepared_statement_column_type(stmt, i);

				if (t != spec->outtypes[i].duck)
				{
					*why = psprintf("DuckDB column %d is %s, %s expected", i + 1,
									gg_duckdb_type_name(t), gg_duckdb_type_name(spec->outtypes[i].duck));
					ok = false;
				}
			}
		}
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		*why = psprintf("DuckDB is unavailable on the coordinator: %s", edata->message);
		ok = false;
	}
	PG_END_TRY();

	if (stmt)
		duckdb_destroy_prepare(&stmt);
	gg_duckdb_disconnect(&conn);
	return ok;
}

/*
 * Try to replace the subtree rooted at `plan` with a region.  Returns the
 * replacement, or NULL when the subtree stays as it is.
 */
static Plan *
try_region(PassContext *ctx, Plan *plan)
{
	GGRegionSpec spec;
	char	   *why = NULL;
	ListCell   *lc;

	memset(&spec, 0, sizeof(spec));
	if (!gg_duckdb_deparse_region(ctx->stmt, plan, &spec))
	{
		decision(ctx, plan, "not eligible: %s", spec.reject ? spec.reject : "?");
		return NULL;
	}
	if (spec.naggs == 0 && spec.nsorts == 0 && strstr(spec.label, "Unique") == NULL)
	{
		decision(ctx, plan, "nothing worth handing to DuckDB (no aggregate, sort or distinct)");
		return NULL;
	}
	if (parent_needs_order(ctx->parent) && !spec.ordered)
	{
		decision(ctx, plan, "its parent needs ordered input");
		return NULL;
	}
	foreach(lc, spec.leaves)
	{
		Plan	   *leaf = (Plan *) lfirst(lc);

		if (IsA(leaf, Motion) && plan->extParam != NULL)
		{
			decision(ctx, plan, "a Motion leaf cannot be rescanned");
			return NULL;
		}
	}
	if (gg_duckdb_mode == GG_DUCKDB_MODE_AUTO && spec.rows_in < (double) gg_duckdb_min_rows)
	{
		decision(ctx, plan, "estimated input rows below gg_duckdb.min_rows");
		elog(DEBUG1, "gg_duckdb: plan node %d: %.0f estimated input rows, below gg_duckdb.min_rows = %d",
			 plan->plan_node_id, spec.rows_in, gg_duckdb_min_rows);
		return NULL;
	}
	if (gg_duckdb_validate_at_plan_time && !validate_region(&spec, &why))
	{
		decision(ctx, plan, "%s", why);
		return NULL;
	}
	decision(ctx, plan, "region [%s]: %s", spec.label, spec.sql);
	elog(DEBUG1, "gg_duckdb: plan node %d: region [%s], %.0f estimated input rows",
		 plan->plan_node_id, spec.label, spec.rows_in);
	return make_region(ctx, plan, &spec);
}

/*
 * After a region is made: its Motion leaves lead to other slices, which
 * get their own regions.
 */
static void
descend_into_motion_leaves(PassContext *ctx, CustomScan *cs)
{
	ListCell   *lc;

	foreach(lc, cs->custom_plans)
	{
		Plan	   *leaf = (Plan *) lfirst(lc);

		if (IsA(leaf, Motion))
			lfirst(lc) = pass_mutator((Node *) leaf, ctx);
	}
}

static Node *
pass_mutator(Node *node, PassContext *ctx)
{
	Plan	   *saved_parent;
	Node	   *result;

	if (node == NULL)
		return NULL;

	if (IsA(node, Motion))
	{
		Motion	   *motion = (Motion *) node;
		int			saved_slice = ctx->slice;

		/* the sender side of a Motion is another slice */
		saved_parent = ctx->parent;
		ctx->parent = (Plan *) node;
		ctx->slice = motion->motionID;
		result = plan_tree_mutator(node, pass_mutator, ctx, false);
		ctx->slice = saved_slice;
		ctx->parent = saved_parent;
		return result;
	}

	if (is_plan_node(node) && slice_runs_on_segments(ctx))
	{
		Plan	   *region = try_region(ctx, (Plan *) node);

		if (region != NULL)
		{
			saved_parent = ctx->parent;
			ctx->parent = region;
			descend_into_motion_leaves(ctx, (CustomScan *) region);
			ctx->parent = saved_parent;
			return (Node *) region;
		}
	}

	saved_parent = ctx->parent;
	if (is_plan_node(node))
		ctx->parent = (Plan *) node;
	result = plan_tree_mutator(node, pass_mutator, ctx, false);
	ctx->parent = saved_parent;
	return result;
}

/* ---------- milestone M1 debug mode: identity regions over SeqScans ---------- */

static Plan *
make_identity_region(PassContext *ctx, Plan *leaf)
{
	GGRegionSpec spec;
	ListCell   *lc;
	StringInfoData sql;
	int			k = 0;

	memset(&spec, 0, sizeof(spec));
	spec.ncols = list_length(leaf->targetlist);
	spec.outtypes = palloc0(sizeof(GGTypeInfo) * Max(spec.ncols, 1));
	foreach(lc, leaf->targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);

		if (!gg_duckdb_type_map(exprType((Node *) te->expr),
								exprTypmod((Node *) te->expr), &spec.outtypes[k]))
			return leaf;
		k++;
	}
	if (spec.ncols == 0)
		return leaf;

	initStringInfo(&sql);
	appendStringInfoString(&sql, "SELECT ");
	for (k = 1; k <= spec.ncols; k++)
		appendStringInfo(&sql, "%sc%d", k > 1 ? ", " : "", k);
	appendStringInfo(&sql, " FROM %s(0)", GG_DUCKDB_LEAF_FUNCTION);
	if (gg_duckdb_debug_region_sql && gg_duckdb_debug_region_sql[0] != '\0')
	{
		resetStringInfo(&sql);
		appendStringInfoString(&sql, gg_duckdb_debug_region_sql);
	}
	spec.sql = sql.data;
	spec.leaves = list_make1(leaf);
	spec.nleaves = 1;
	spec.params = NIL;
	spec.flags = 0;
	spec.label = "identity over SeqScan";
	spec.rows_in = leaf->plan_rows;
	return make_region(ctx, leaf, &spec);
}

static Node *
wrap_mutator(Node *node, PassContext *ctx)
{
	if (node == NULL)
		return NULL;

	switch (nodeTag(node))
	{
		case T_Motion:
			{
				Motion	   *motion = (Motion *) node;
				int			saved = ctx->slice;
				Node	   *result;

				ctx->slice = motion->motionID;
				result = plan_tree_mutator(node, wrap_mutator, ctx, false);
				ctx->slice = saved;
				return result;
			}
		case T_SeqScan:
			if (slice_runs_on_segments(ctx))
				return (Node *) make_identity_region(ctx, (Plan *) node);
			break;
		default:
			break;
	}
	return plan_tree_mutator(node, wrap_mutator, ctx, false);
}

/* ---------- the hook ---------- */

static PlannedStmt *
gg_duckdb_post_planner(PlannedStmt *stmt, Query *parse, int cursorOptions,
					   ParamListInfo boundParams)
{
	PassContext ctx;

	if (prev_post_planner_hook)
		stmt = (*prev_post_planner_hook) (stmt, parse, cursorOptions, boundParams);

	if (gg_duckdb_mode == GG_DUCKDB_MODE_OFF ||
		Gp_role != GP_ROLE_DISPATCH ||
		stmt->commandType != CMD_SELECT ||
		stmt->planTree == NULL)
		return stmt;

	memset(&ctx, 0, sizeof(ctx));
	exec_init_plan_tree_base(&ctx.base, stmt);
	ctx.stmt = stmt;
	ctx.slice = 0;
	ctx.parent = NULL;
	ctx.next_plan_node_id = 1;
	max_plan_node_id_walker((Node *) stmt->planTree, &ctx);

	if (gg_duckdb_debug_wrap == GG_DUCKDB_WRAP_SCANS)
		stmt->planTree = (Plan *) wrap_mutator((Node *) stmt->planTree, &ctx);
	else
		stmt->planTree = (Plan *) pass_mutator((Node *) stmt->planTree, &ctx);

	if (ctx.wrapped > 0)
		elog(DEBUG1, "gg_duckdb: %d region(s) planned", ctx.wrapped);
	return stmt;
}

void
gg_duckdb_install_planner_hook(void)
{
	prev_post_planner_hook = post_planner_hook;
	post_planner_hook = gg_duckdb_post_planner;
}
