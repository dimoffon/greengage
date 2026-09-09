/*-------------------------------------------------------------------------
 *
 * pass.c
 *	  The planner pass: runs on the finished PlannedStmt of both optimizers
 *	  (post_planner_hook) and replaces eligible subtrees with DuckDB regions.
 *
 * Milestone M1: only the debug mode gg_duckdb.debug_wrap = scans exists.  It
 * wraps every SeqScan of a segment slice into an identity region
 * (SELECT c1..cN FROM gg_leaf(0)), which exercises dispatch, execution,
 * conversion, EXPLAIN, squelch and rescan before any deparser exists.
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

typedef struct WrapContext
{
	plan_tree_base_prefix base;	/* required by the plan tree walkers */
	PlannedStmt *stmt;
	int			slice;			/* slice of the node being visited */
	int			next_plan_node_id;
	int			wrapped;
} WrapContext;

static bool
max_plan_node_id_walker(Node *node, WrapContext *ctx)
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
slice_runs_on_segments(WrapContext *ctx)
{
	PlanSlice  *slice;

	if (ctx->stmt->numSlices <= 0 || ctx->slice < 0 || ctx->slice >= ctx->stmt->numSlices)
		return false;
	slice = &ctx->stmt->slices[ctx->slice];
	return slice->gangType == GANGTYPE_PRIMARY_READER ||
		slice->gangType == GANGTYPE_PRIMARY_WRITER ||
		slice->gangType == GANGTYPE_SINGLETON_READER;
}

/*
 * Wrap `leaf` into a region whose DuckDB query is the identity over the
 * leaf's output, or return the leaf itself when its output has a type a
 * region cannot carry.
 */
static Plan *
make_identity_region(WrapContext *ctx, Plan *leaf)
{
	List	   *tlist = leaf->targetlist;
	List	   *colnames = NIL;
	List	   *scan_tlist = NIL;
	List	   *out_tlist = NIL;
	RangeTblEntry *rte;
	CustomScan *cs;
	StringInfoData sql;
	ListCell   *lc;
	int			rteidx;
	int			k;
	GGTypeInfo	ti;

	if (tlist == NIL)
		return leaf;
	foreach(lc, tlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);

		if (!gg_duckdb_type_map(exprType((Node *) te->expr),
								exprTypmod((Node *) te->expr), &ti))
			return leaf;
	}

	/*
	 * A synthetic range-table entry names the region's output for EXPLAIN:
	 * INDEX_VAR Vars are resolved through custom_scan_tlist, whose Vars must
	 * point at a real entry.
	 */
	rteidx = list_length(ctx->stmt->rtable) + 1;
	initStringInfo(&sql);
	appendStringInfoString(&sql, "SELECT ");
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
		appendStringInfo(&sql, "%sc%d", k > 1 ? ", " : "", k);
		k++;
	}
	appendStringInfo(&sql, " FROM %s(0)", GG_DUCKDB_LEAF_FUNCTION);
	if (gg_duckdb_debug_region_sql && gg_duckdb_debug_region_sql[0] != '\0')
	{
		resetStringInfo(&sql);
		appendStringInfoString(&sql, gg_duckdb_debug_region_sql);
	}

	/*
	 * A named-tuplestore entry, as pg_duckdb uses: the deparser expands it
	 * through its column lists, so EXPLAIN VERBOSE can name the columns.
	 * Nothing ever scans it.
	 */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_NAMEDTUPLESTORE;
	rte->enrname = "gg_duckdb_region";
	rte->enrtuples = leaf->plan_rows;
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
	cs->flags = 0;
	cs->custom_plans = list_make1(leaf);
	cs->custom_exprs = NIL;
	cs->custom_private = gg_duckdb_make_private(sql.data, 0, 1,
												psprintf("identity over %s",
														 nodeTag(leaf) == T_SeqScan ? "SeqScan" : "leaf"));
	cs->custom_scan_tlist = scan_tlist;
	cs->custom_relids = bms_make_singleton(rteidx);
	cs->methods = &gg_duckdb_scan_methods;
	cs->scan.scanrelid = 0;
	cs->scan.plan.targetlist = out_tlist;
	cs->scan.plan.qual = NIL;
	cs->scan.plan.lefttree = NULL;
	cs->scan.plan.righttree = NULL;
	cs->scan.plan.initPlan = NIL;
	cs->scan.plan.startup_cost = leaf->startup_cost;
	cs->scan.plan.total_cost = leaf->total_cost;
	cs->scan.plan.plan_rows = leaf->plan_rows;
	cs->scan.plan.plan_width = leaf->plan_width;
	cs->scan.plan.plan_node_id = ctx->next_plan_node_id++;
	cs->scan.plan.extParam = bms_copy(leaf->extParam);
	cs->scan.plan.allParam = bms_copy(leaf->allParam);
	cs->scan.plan.flow = leaf->flow;

	ctx->wrapped++;
	return (Plan *) cs;
}

static Node *
wrap_mutator(Node *node, WrapContext *ctx)
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

				/* the sender side of a Motion is another slice */
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

static PlannedStmt *
gg_duckdb_post_planner(PlannedStmt *stmt, Query *parse, int cursorOptions,
					   ParamListInfo boundParams)
{
	if (prev_post_planner_hook)
		stmt = (*prev_post_planner_hook) (stmt, parse, cursorOptions, boundParams);

	if (gg_duckdb_mode == GG_DUCKDB_MODE_OFF ||
		Gp_role != GP_ROLE_DISPATCH ||
		stmt->commandType != CMD_SELECT ||
		stmt->planTree == NULL)
		return stmt;

	if (gg_duckdb_debug_wrap == GG_DUCKDB_WRAP_SCANS)
	{
		WrapContext ctx;

		memset(&ctx, 0, sizeof(ctx));
		exec_init_plan_tree_base(&ctx.base, stmt);
		ctx.stmt = stmt;
		ctx.slice = 0;
		ctx.next_plan_node_id = 1;
		max_plan_node_id_walker((Node *) stmt->planTree, &ctx);

		stmt->planTree = (Plan *) wrap_mutator((Node *) stmt->planTree, &ctx);
		if (ctx.wrapped > 0)
			elog(DEBUG1, "gg_duckdb: wrapped %d scan(s) into identity regions", ctx.wrapped);
	}
	return stmt;
}

void
gg_duckdb_install_planner_hook(void)
{
	prev_post_planner_hook = post_planner_hook;
	post_planner_hook = gg_duckdb_post_planner;
}
