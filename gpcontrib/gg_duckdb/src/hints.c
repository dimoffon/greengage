/*-------------------------------------------------------------------------
 *
 * hints.c
 *	  DuckDB(t1 t2 ...) and NoDuckDB(t1 t2 ...) plan hints.
 *
 * pg_hint_plan parses the hints (HINT_TYPE_DUCKDB in optimizer/hints.h);
 * the planner pass asks here whether a candidate region is forced or
 * forbidden.  A region is addressed by the aliases of its scan leaves:
 * DuckDB(S) forces every eligible region whose alias set covers S, which
 * in a top-down pass is the maximal one; NoDuckDB(S) forbids every region
 * that touches S; NoDuckDB wins over DuckDB; an empty list means the whole
 * query.  Hints beat the cost gate but not gg_duckdb.mode = off, and they
 * never make an ineligible subtree eligible.  A DuckDB hint that no region
 * honoured is reported as a NOTICE, or an ERROR under gg_duckdb.strict.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/hints.h"
#include "optimizer/orca.h"
#include "parser/parsetree.h"

#include "gg_duckdb/gg_duckdb.h"

typedef struct GGHint
{
	List	   *aliases;		/* relation aliases named, NIL = whole query */
	bool		negative;
	bool		used;
	const char *text;			/* for messages */
} GGHint;

struct GGHints
{
	List	   *hints;			/* of GGHint */
};

static void
unhonoured(const GGHint *h, const char *why)
{
	ereport(gg_duckdb_strict ? ERROR : NOTICE,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("gg_duckdb: hint %s could not be honoured: %s", h->text, why)));
}

static char *
hint_text(const DuckDBHint *hint)
{
	StringInfoData s;
	int			i;

	initStringInfo(&s);
	appendStringInfo(&s, "%s(", hint->base.keyword);
	for (i = 0; i < hint->nrels; i++)
		appendStringInfo(&s, "%s%s", i > 0 ? " " : "", hint->relnames[i]);
	appendStringInfoChar(&s, ')');
	return s.data;
}

static bool
alias_in_rtable(const char *alias, List *rtable)
{
	ListCell   *lc;

	foreach(lc, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->eref && rte->eref->aliasname &&
			strcmp(rte->eref->aliasname, alias) == 0)
			return true;
	}
	return false;
}

/*
 * Parse the query's hints through pg_hint_plan and keep the DuckDB ones.
 * NULL when there are none (or pg_hint_plan is not loaded).
 */
GGHints *
gg_duckdb_hints_collect(Query *parse, PlannedStmt *stmt)
{
	HintState  *hstate;
	GGHints    *hints = NULL;
	int			i;

	if (plan_hint_hook == NULL || parse == NULL)
		return NULL;
	hstate = (HintState *) plan_hint_hook(parse);
	if (hstate == NULL || hstate->num_hints[HINT_TYPE_DUCKDB] == 0)
		return NULL;

	for (i = 0; i < hstate->num_hints[HINT_TYPE_DUCKDB]; i++)
	{
		DuckDBHint *dh = hstate->duckdb_hints[i];
		GGHint	   *h;
		int			k;
		bool		ok = true;

		/* an earlier duplicate of the same hint, or a parse error */
		if (dh->base.state != HINT_STATE_NOTUSED && dh->base.state != HINT_STATE_USED)
			continue;

		h = palloc0(sizeof(GGHint));
		h->negative = dh->negative;
		h->text = hint_text(dh);
		for (k = 0; k < dh->nrels; k++)
		{
			if (!alias_in_rtable(dh->relnames[k], stmt->rtable))
			{
				unhonoured(h, psprintf("\"%s\" is not a relation of the query", dh->relnames[k]));
				ok = false;
				break;
			}
			h->aliases = lappend(h->aliases, pstrdup(dh->relnames[k]));
		}
		if (!ok)
			continue;
		if (hints == NULL)
			hints = palloc0(sizeof(GGHints));
		hints->hints = lappend(hints->hints, h);
	}
	return hints;
}

static bool
list_contains_string(List *list, const char *s)
{
	ListCell   *lc;

	foreach(lc, list)
		if (strcmp((const char *) lfirst(lc), s) == 0)
			return true;
	return false;
}

/*
 * Aliases of the scan leaves of a region, in plan order.
 */
List *
gg_duckdb_region_aliases(PlannedStmt *stmt, List *leaves)
{
	List	   *aliases = NIL;
	ListCell   *lc;

	foreach(lc, leaves)
	{
		Plan	   *leaf = (Plan *) lfirst(lc);
		Index		relid = 0;

		switch (nodeTag(leaf))
		{
			case T_SeqScan:
			case T_DynamicSeqScan:
			case T_SampleScan:
			case T_IndexScan:
			case T_DynamicIndexScan:
			case T_IndexOnlyScan:
			case T_DynamicIndexOnlyScan:
			case T_BitmapHeapScan:
			case T_DynamicBitmapHeapScan:
			case T_TidScan:
			case T_SubqueryScan:
			case T_FunctionScan:
			case T_ValuesScan:
			case T_CteScan:
			case T_ForeignScan:
			case T_DynamicForeignScan:
			case T_TableFuncScan:
				relid = ((Scan *) leaf)->scanrelid;
				break;
			default:
				break;
		}
		if (relid > 0 && relid <= list_length(stmt->rtable))
		{
			RangeTblEntry *rte = rt_fetch(relid, stmt->rtable);

			if (rte->eref && rte->eref->aliasname &&
				!list_contains_string(aliases, rte->eref->aliasname))
				aliases = lappend(aliases, rte->eref->aliasname);
		}
	}
	return aliases;
}

/*
 * What the hints say about a region over `aliases`.  Positive hints that
 * apply are marked as used.
 */
GGHintVerdict
gg_duckdb_hints_verdict(GGHints *hints, List *aliases)
{
	ListCell   *lc;
	bool		force = false;

	if (hints == NULL)
		return GG_HINT_NONE;

	foreach(lc, hints->hints)
	{
		GGHint	   *h = (GGHint *) lfirst(lc);

		if (!h->negative)
			continue;
		if (h->aliases == NIL)
			return GG_HINT_FORBID;
		{
			ListCell   *ac;

			foreach(ac, h->aliases)
				if (list_contains_string(aliases, (const char *) lfirst(ac)))
					return GG_HINT_FORBID;
		}
	}
	foreach(lc, hints->hints)
	{
		GGHint	   *h = (GGHint *) lfirst(lc);
		ListCell   *ac;
		bool		covered = true;

		if (h->negative)
			continue;
		foreach(ac, h->aliases)
			if (!list_contains_string(aliases, (const char *) lfirst(ac)))
			{
				covered = false;
				break;
			}
		if (covered)
		{
			h->used = true;
			force = true;
		}
	}
	return force ? GG_HINT_FORCE : GG_HINT_NONE;
}

/*
 * After the pass: every DuckDB hint no region honoured.
 */
void
gg_duckdb_hints_report(GGHints *hints)
{
	ListCell   *lc;

	if (hints == NULL)
		return;
	foreach(lc, hints->hints)
	{
		GGHint	   *h = (GGHint *) lfirst(lc);

		if (!h->negative && !h->used)
			unhonoured(h, "no eligible DuckDB region covers these relations");
	}
}
