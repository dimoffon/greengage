/*-------------------------------------------------------------------------
 *
 * deparse.c
 *	  Turn a Motion-free plan subtree into a DuckDB query, or say why not.
 *
 * "Eligible" means "this code generates": one walk both decides and emits,
 * so a region is never planned that cannot be expressed.  The rules are a
 * whitelist of node kinds, types, operators, functions and aggregates whose
 * DuckDB semantics are known to equal PostgreSQL's; everything else turns
 * the node into a leaf (an ordinary plan node pulled through gg_leaf) or
 * rejects the region.
 *
 * Shape of a region (milestone M2):
 *
 *     [Limit] [Unique] [Sort] base           -- the "top chain", hoisted into
 *                                               one SELECT [DISTINCT] ...
 *                                               ORDER BY ... LIMIT/OFFSET
 *     base := Result(base) | Agg(base) | Material(base) | leaf
 *
 * Every node's output is a subquery whose columns are c1..cN in resno
 * order; Vars of interior nodes are OUTER_VAR references into the child,
 * which is the setrefs form both optimizers deliver at the hook point.
 * Constants become parameters $n, bound on the QE from the Const list.
 * Every non-trivial expression is wrapped in CAST(... AS <its PG type>),
 * so DuckDB's result types equal PostgreSQL's.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include <locale.h>

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "utils/builtins.h"
#include "optimizer/walkers.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "optimizer/optimizer.h"

#include "gg_duckdb/gg_duckdb.h"

/* the output columns of a deparsed node, in resno order */
typedef struct NodeCols
{
	int			ncols;
	GGTypeInfo *types;
} NodeCols;

typedef struct DeparseCtx
{
	GGRegionSpec *spec;
	int			next_alias;
	NodeCols   *child;			/* columns of the outer child of the node being deparsed */
	const char *outer_alias;	/* its alias in the FROM clause, or NULL */
	NodeCols   *inner;			/* columns of the inner child (joins), or NULL */
	const char *inner_alias;
	bool		in_agg;			/* Aggrefs are allowed (Agg tlist / HAVING) */
} DeparseCtx;

/* Record the reason and fail. */
#define REJECT(ctx, ...) \
	do { \
		if ((ctx)->spec->reject == NULL) \
			(ctx)->spec->reject = psprintf(__VA_ARGS__); \
		return false; \
	} while (0)

static bool deparse_base(DeparseCtx *ctx, Plan *plan, StringInfo out, NodeCols *cols);
static bool deparse_expr(DeparseCtx *ctx, Node *node, StringInfo out, GGTypeInfo *type);

/* ---------- small helpers ---------- */

const char *
gg_duckdb_type_sql(const GGTypeInfo *ti)
{
	if (gg_duckdb_is_numeric_state(ti))
		return psprintf("STRUCT(s DECIMAL(38,%d), n BIGINT)", ti->scale);
	switch (ti->duck)
	{
		case DUCKDB_TYPE_BOOLEAN: return "BOOLEAN";
		case DUCKDB_TYPE_SMALLINT: return "SMALLINT";
		case DUCKDB_TYPE_INTEGER: return "INTEGER";
		case DUCKDB_TYPE_BIGINT: return "BIGINT";
		case DUCKDB_TYPE_FLOAT: return "FLOAT";
		case DUCKDB_TYPE_DOUBLE: return "DOUBLE";
		case DUCKDB_TYPE_DECIMAL: return psprintf("DECIMAL(%d,%d)", ti->width, ti->scale);
		case DUCKDB_TYPE_VARCHAR: return "VARCHAR";
		case DUCKDB_TYPE_BLOB: return "BLOB";
		case DUCKDB_TYPE_DATE: return "DATE";
		case DUCKDB_TYPE_TIMESTAMP: return "TIMESTAMP";
		case DUCKDB_TYPE_TIMESTAMP_TZ: return "TIMESTAMP WITH TIME ZONE";
		case DUCKDB_TYPE_INTERVAL: return "INTERVAL";
		case DUCKDB_TYPE_UUID: return "UUID";
		default: return "?";
	}
}

/*
 * Whether comparisons under this collation are byte order, which is what
 * DuckDB does.  Equality is bytewise under every deterministic collation in
 * PostgreSQL 12, so only ordering comparisons need this.
 */
static bool
collation_is_binary(Oid collation)
{
	const char *lc;

	if (collation == C_COLLATION_OID || collation == POSIX_COLLATION_OID)
		return true;
	if (collation != DEFAULT_COLLATION_OID && OidIsValid(collation))
		return false;
	lc = setlocale(LC_COLLATE, NULL);
	return lc != NULL && (strcmp(lc, "C") == 0 || strcmp(lc, "POSIX") == 0);
}

static bool
type_is_text(Oid typid)
{
	return typid == TEXTOID || typid == VARCHAROID || typid == BPCHAROID;
}

static bool
type_is_integer(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID;
}

static bool
type_is_float(Oid typid)
{
	return typid == FLOAT4OID || typid == FLOAT8OID;
}

static bool
type_is_comparable_family(Oid a, Oid b)
{
	if ((type_is_integer(a) || type_is_float(a) || a == NUMERICOID) &&
		(type_is_integer(b) || type_is_float(b) || b == NUMERICOID))
		return true;
	if (type_is_text(a) && type_is_text(b))
		return true;
	return a == b && (a == BOOLOID || a == DATEOID || a == TIMESTAMPOID ||
					  a == TIMESTAMPTZOID || a == INTERVALOID || a == UUIDOID ||
					  a == BYTEAOID);
}

/* The DuckDB type of a PG expression type, or reject. */
static bool
map_type(DeparseCtx *ctx, Oid typid, int32 typmod, GGTypeInfo *ti)
{
	if (!gg_duckdb_type_map(typid, typmod, ti))
		REJECT(ctx, "type %s is not carried", format_type_with_typemod(typid, typmod));
	return true;
}

/*
 * A numeric constant has no typmod; its DECIMAL width and scale come from
 * its digits.
 */
static void
numeric_const_shape(const char *text, uint8 *width, uint8 *scale)
{
	int			digits = 0,
				frac = 0;
	bool		infrac = false;
	const char *p;

	for (p = text; *p; p++)
	{
		if (*p == '.')
			infrac = true;
		else if (*p >= '0' && *p <= '9')
		{
			digits++;
			if (infrac)
				frac++;
		}
	}
	*width = (uint8) Max(Min(digits, 38), 1);
	*scale = (uint8) Min(frac, 38);
}

/* ---------- expressions ---------- */

/*
 * Emit a Const as a bound parameter with an explicit cast.  NULLs are
 * emitted as typed NULLs.
 */
static bool
deparse_const(DeparseCtx *ctx, Const *c, StringInfo out, GGTypeInfo *type)
{
	if (c->consttype == NUMERICOID && !c->constisnull)
	{
		char	   *text = DatumGetCString(DirectFunctionCall1(numeric_out, c->constvalue));

		if (strcmp(text, "NaN") == 0)
			REJECT(ctx, "numeric NaN constant");
		memset(type, 0, sizeof(*type));
		type->typid = NUMERICOID;
		type->typmod = -1;
		type->duck = DUCKDB_TYPE_DECIMAL;
		numeric_const_shape(text, &type->width, &type->scale);
	}
	else if (!map_type(ctx, c->consttype, c->consttypmod, type))
		return false;

	if (c->constisnull)
	{
		appendStringInfo(out, "CAST(NULL AS %s)", gg_duckdb_type_sql(type));
		return true;
	}
	ctx->spec->params = lappend(ctx->spec->params, copyObject(c));
	appendStringInfo(out, "CAST($%d AS %s)", list_length(ctx->spec->params),
					 gg_duckdb_type_sql(type));
	return true;
}

static bool
deparse_var(DeparseCtx *ctx, Var *var, StringInfo out, GGTypeInfo *type)
{
	NodeCols   *cols;
	const char *alias;

	if (var->varno == OUTER_VAR)
	{
		cols = ctx->child;
		alias = ctx->outer_alias;
	}
	else if (var->varno == INNER_VAR)
	{
		cols = ctx->inner;
		alias = ctx->inner_alias;
	}
	else
		REJECT(ctx, "unexpected Var reference (varno %d)", (int) var->varno);
	if (cols == NULL || var->varattno < 1 || var->varattno > cols->ncols)
		REJECT(ctx, "Var attno %d out of range", (int) var->varattno);
	if (var->varlevelsup != 0)
		REJECT(ctx, "outer-level Var");
	*type = cols->types[var->varattno - 1];
	if (alias)
		appendStringInfo(out, "%s.c%d", alias, (int) var->varattno);
	else
		appendStringInfo(out, "c%d", (int) var->varattno);
	return true;
}

/*
 * Binary operators whose DuckDB semantics equal PostgreSQL's on the
 * whitelisted types.  `a` and `b` are already deparsed.
 */
/*
 * The DECIMAL shape of an operand of numeric arithmetic: its own for a
 * DECIMAL, the digits of its type for an integer.
 */
static bool
decimal_shape(const GGTypeInfo *t, int *width, int *scale)
{
	if (t->duck == DUCKDB_TYPE_DECIMAL)
	{
		*width = t->width;
		*scale = t->scale;
		return true;
	}
	*scale = 0;
	switch (t->duck)
	{
		case DUCKDB_TYPE_SMALLINT:
			*width = 5;
			return true;
		case DUCKDB_TYPE_INTEGER:
			*width = 10;
			return true;
		case DUCKDB_TYPE_BIGINT:
			*width = 19;
			return true;
		default:
			return false;
	}
}

static bool
deparse_opexpr(DeparseCtx *ctx, OpExpr *op, StringInfo out, GGTypeInfo *type)
{
	HeapTuple	tup;
	Form_pg_operator opform;
	char	   *name;
	Oid			ltype,
				rtype;
	StringInfoData a,
				b;
	GGTypeInfo	ta,
				tb;
	bool		ok;

	if (list_length(op->args) != 2 && list_length(op->args) != 1)
		REJECT(ctx, "operator with %d arguments", list_length(op->args));
	/* numeric arithmetic has no typmod: its DECIMAL shape follows from the operands */
	if (op->opresulttype != NUMERICOID && !map_type(ctx, op->opresulttype, -1, type))
		return false;

	tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(op->opno));
	if (!HeapTupleIsValid(tup))
		REJECT(ctx, "operator %u not found", op->opno);
	opform = (Form_pg_operator) GETSTRUCT(tup);
	name = pstrdup(NameStr(opform->oprname));
	ltype = opform->oprleft;
	rtype = opform->oprright;
	ok = opform->oprnamespace == PG_CATALOG_NAMESPACE;
	ReleaseSysCache(tup);
	if (!ok)
		REJECT(ctx, "operator %s is not a builtin", name);

	if (list_length(op->args) == 1)
	{
		/* unary minus */
		initStringInfo(&a);
		if (strcmp(name, "-") != 0 || !(type_is_integer(rtype) || type_is_float(rtype)))
			REJECT(ctx, "unary operator %s", name);
		if (!deparse_expr(ctx, (Node *) linitial(op->args), &a, &ta))
			return false;
		appendStringInfo(out, "CAST(-(%s) AS %s)", a.data, gg_duckdb_type_sql(type));
		return true;
	}

	initStringInfo(&a);
	initStringInfo(&b);
	if (!deparse_expr(ctx, (Node *) linitial(op->args), &a, &ta) ||
		!deparse_expr(ctx, (Node *) lsecond(op->args), &b, &tb))
		return false;

	if (strcmp(name, "=") == 0 || strcmp(name, "<>") == 0)
	{
		if (!type_is_comparable_family(ltype, rtype))
			REJECT(ctx, "operator %s on %s and %s", name, format_type_be(ltype), format_type_be(rtype));
		appendStringInfo(out, "((%s) %s (%s))", a.data, name, b.data);
		return true;
	}
	if (strcmp(name, "<") == 0 || strcmp(name, "<=") == 0 ||
		strcmp(name, ">") == 0 || strcmp(name, ">=") == 0)
	{
		if (!type_is_comparable_family(ltype, rtype))
			REJECT(ctx, "operator %s on %s and %s", name, format_type_be(ltype), format_type_be(rtype));
		if (type_is_text(ltype) && !collation_is_binary(op->inputcollid))
			REJECT(ctx, "text ordering under a non-C collation");
		if (ltype == BYTEAOID)
			REJECT(ctx, "bytea ordering");
		appendStringInfo(out, "((%s) %s (%s))", a.data, name, b.data);
		return true;
	}
	if (strcmp(name, "+") == 0 || strcmp(name, "-") == 0 || strcmp(name, "*") == 0)
	{
		bool		numeric_ok =
		(type_is_integer(ltype) && type_is_integer(rtype)) ||
		(type_is_float(ltype) && type_is_float(rtype));
		bool		datetime_ok =
		(strcmp(name, "*") != 0 &&
		 ((ltype == DATEOID && rtype == INT4OID) ||
		  (ltype == TIMESTAMPOID && rtype == INTERVALOID) ||
		  (ltype == INTERVALOID && rtype == INTERVALOID) ||
		  (strcmp(name, "-") == 0 && ltype == TIMESTAMPOID && rtype == TIMESTAMPOID)));

		if (op->opresulttype == NUMERICOID)
		{
			/*
			 * Exact decimal arithmetic in both engines, so only the shape of
			 * the result must be derived: DuckDB's rules for DECIMAL
			 * addition and multiplication, an integer operand counted as a
			 * DECIMAL of its digits.  A product wider than 38 digits is not
			 * representable and is declined.
			 */
			int			w1,
						s1,
						w2,
						s2,
						w,
						sc;

			if (!decimal_shape(&ta, &w1, &s1) || !decimal_shape(&tb, &w2, &s2))
				REJECT(ctx, "operator %s on %s and %s", name, format_type_be(ltype), format_type_be(rtype));
			if (strcmp(name, "*") == 0)
			{
				w = w1 + w2;
				sc = s1 + s2;
			}
			else
			{
				sc = Max(s1, s2);
				w = Max(w1 - s1, w2 - s2) + sc + 1;
			}
			if (w > 38 || sc > 38)
				REJECT(ctx, "numeric %s needs more than 38 digits", name);
			memset(type, 0, sizeof(*type));
			type->typid = NUMERICOID;
			type->typmod = -1;
			type->duck = DUCKDB_TYPE_DECIMAL;
			type->width = (uint8) w;
			type->scale = (uint8) sc;
			appendStringInfo(out, "CAST(CAST(%s AS DECIMAL(%d,%d)) %s CAST(%s AS DECIMAL(%d,%d)) AS %s)",
							 a.data, w1, s1, name, b.data, w2, s2, gg_duckdb_type_sql(type));
			return true;
		}
		if (!numeric_ok && !datetime_ok)
			REJECT(ctx, "operator %s on %s and %s", name, format_type_be(ltype), format_type_be(rtype));
		appendStringInfo(out, "CAST((%s) %s (%s) AS %s)", a.data, name, b.data,
						 gg_duckdb_type_sql(type));
		return true;
	}
	if (strcmp(name, "/") == 0 || strcmp(name, "%") == 0)
	{
		const char *duckop = name;

		if (type_is_integer(ltype) && type_is_integer(rtype))
		{
			if (strcmp(name, "/") == 0)
				duckop = "//";
		}
		else if (!(strcmp(name, "/") == 0 && type_is_float(ltype) && type_is_float(rtype)))
			REJECT(ctx, "operator %s on %s and %s", name, format_type_be(ltype), format_type_be(rtype));
		/* DuckDB returns NULL or infinity where PostgreSQL raises an error */
		appendStringInfo(out,
						 "CAST((CASE WHEN (%s) = 0 THEN error('division by zero') ELSE (%s) %s (%s) END) AS %s)",
						 b.data, a.data, duckop, b.data, gg_duckdb_type_sql(type));
		return true;
	}
	if (strcmp(name, "||") == 0 && type_is_text(ltype) && type_is_text(rtype))
	{
		appendStringInfo(out, "((%s) || (%s))", a.data, b.data);
		return true;
	}
	if ((strcmp(name, "~~") == 0 || strcmp(name, "!~~") == 0) &&
		type_is_text(ltype) && type_is_text(rtype))
	{
		appendStringInfo(out, "((%s) %s (%s) ESCAPE '\\')", a.data,
						 name[0] == '!' ? "NOT LIKE" : "LIKE", b.data);
		return true;
	}
	REJECT(ctx, "operator %s on %s and %s", name, format_type_be(ltype), format_type_be(rtype));
}

/* Builtin functions and casts with equal semantics. */
static bool
deparse_funcexpr(DeparseCtx *ctx, FuncExpr *f, StringInfo out, GGTypeInfo *type)
{
	HeapTuple	tup;
	Form_pg_proc proc;
	char	   *name;
	bool		builtin;
	int			nargs = list_length(f->args);
	Oid			argtype = nargs > 0 ? exprType((Node *) linitial(f->args)) : InvalidOid;
	StringInfoData a;
	GGTypeInfo	ta;

	if (f->funcretset)
		REJECT(ctx, "set-returning function");
	if (!map_type(ctx, f->funcresulttype, -1, type))
		return false;

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(f->funcid));
	if (!HeapTupleIsValid(tup))
		REJECT(ctx, "function %u not found", f->funcid);
	proc = (Form_pg_proc) GETSTRUCT(tup);
	name = pstrdup(NameStr(proc->proname));
	builtin = proc->pronamespace == PG_CATALOG_NAMESPACE && proc->provolatile == PROVOLATILE_IMMUTABLE;
	ReleaseSysCache(tup);
	if (!builtin)
		REJECT(ctx, "function %s is not an immutable builtin", name);

	if (nargs == 1)
	{
		initStringInfo(&a);
		if (!deparse_expr(ctx, (Node *) linitial(f->args), &a, &ta))
			return false;

		/* widening casts among integers and floats */
		if ((strcmp(name, "int4") == 0 && argtype == INT2OID) ||
			(strcmp(name, "int8") == 0 && (argtype == INT2OID || argtype == INT4OID)) ||
			(strcmp(name, "float8") == 0 && (argtype == INT2OID || argtype == INT4OID ||
											 argtype == INT8OID || argtype == FLOAT4OID)) ||
			(strcmp(name, "float4") == 0 && (argtype == INT2OID || argtype == INT4OID)))
		{
			appendStringInfo(out, "CAST(%s AS %s)", a.data, gg_duckdb_type_sql(type));
			return true;
		}
		if (strcmp(name, "abs") == 0 && (type_is_integer(argtype) || type_is_float(argtype)))
		{
			appendStringInfo(out, "CAST(abs(%s) AS %s)", a.data, gg_duckdb_type_sql(type));
			return true;
		}
		if ((strcmp(name, "length") == 0 || strcmp(name, "char_length") == 0) &&
			type_is_text(argtype))
		{
			appendStringInfo(out, "CAST(length(%s) AS INTEGER)", a.data);
			return true;
		}
		if (strcmp(name, "text") == 0 && type_is_text(argtype))
		{
			appendStringInfoString(out, a.data);
			return true;
		}
	}
	REJECT(ctx, "function %s(%d args)", name, nargs);
}

/*
 * The partial phase of sum(numeric) or avg(numeric): PostgreSQL hands its
 * final phase a serialised aggregate state (a bytea).  DuckDB computes the
 * exact sum and the count, and the region packs them into that state on
 * output (types.c); the final phase either runs on the standard executor
 * or in a region that adds the packed sums up.
 */
static bool
deparse_numeric_state(DeparseCtx *ctx, Aggref *agg, const char *arg, int scale,
					  StringInfo out, GGTypeInfo *type)
{
	StringInfoData f;

	initStringInfo(&f);
	if (agg->aggfilter)
	{
		GGTypeInfo	tf;
		StringInfoData fe;

		initStringInfo(&fe);
		if (!deparse_expr(ctx, (Node *) agg->aggfilter, &fe, &tf))
			return false;
		appendStringInfo(&f, " FILTER (WHERE %s)", fe.data);
	}
	if (agg->aggtype != BYTEAOID)
		REJECT(ctx, "partial numeric aggregate declares %s", format_type_be(agg->aggtype));
	gg_duckdb_numeric_state_type(type, scale);
	appendStringInfo(out, "struct_pack(s := CAST(sum(%s)%s AS DECIMAL(38,%d)), n := count(%s)%s)",
					 arg, f.data, scale, arg, f.data);
	return true;
}

static bool
deparse_aggref(DeparseCtx *ctx, Aggref *agg, StringInfo out, GGTypeInfo *type)
{
	HeapTuple	tup;
	Form_pg_proc proc;
	char	   *name;
	bool		builtin;
	StringInfoData a;
	GGTypeInfo	ta;
	Oid			argtype = InvalidOid;
	const char *cast;

	bool		partial = agg->aggsplit == AGGSPLIT_INITIAL_SERIAL;
	bool		final = agg->aggsplit == AGGSPLIT_FINAL_DESERIAL;

	if (!ctx->in_agg)
		REJECT(ctx, "aggregate outside an Agg node");
	if (agg->aggsplit != AGGSPLIT_SIMPLE && !partial && !final)
		REJECT(ctx, "aggregate split mode %d", (int) agg->aggsplit);
	if (agg->aggorder != NIL || agg->aggdirectargs != NIL)
		REJECT(ctx, "ordered-set or ordered aggregate");
	if (agg->aggkind != AGGKIND_NORMAL)
		REJECT(ctx, "non-normal aggregate");
	if (list_length(agg->args) > 1)
		REJECT(ctx, "aggregate with %d arguments", list_length(agg->args));
	if ((partial || final) && agg->aggdistinct != NIL)
		REJECT(ctx, "DISTINCT in a split aggregate");
	if (final && (agg->aggfilter != NULL || list_length(agg->args) != 1))
		REJECT(ctx, "unexpected shape of a final aggregate");

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(agg->aggfnoid));
	if (!HeapTupleIsValid(tup))
		REJECT(ctx, "aggregate %u not found", agg->aggfnoid);
	proc = (Form_pg_proc) GETSTRUCT(tup);
	name = pstrdup(NameStr(proc->proname));
	builtin = proc->pronamespace == PG_CATALOG_NAMESPACE;
	ReleaseSysCache(tup);
	if (!builtin)
		REJECT(ctx, "aggregate %s is not a builtin", name);

	initStringInfo(&a);
	if (!final && (agg->aggstar || agg->args == NIL))
	{
		/*
		 * count(*): ORCA leaves aggstar unset and the argument list empty.
		 * A final count(*) keeps aggstar but its argument is the partial
		 * count, which is what it must sum.
		 */
		if (strcmp(name, "count") != 0)
			REJECT(ctx, "aggregate %s without arguments", name);
		appendStringInfoChar(&a, '*');
	}
	else
	{
		TargetEntry *te = (TargetEntry *) linitial(agg->args);

		argtype = exprType((Node *) te->expr);
		if (!deparse_expr(ctx, (Node *) te->expr, &a, &ta))
			return false;
	}

	/*
	 * The result type as PostgreSQL declares it, with DuckDB's shape.  A
	 * final aggregate's single argument is the partial result column; only
	 * aggregates whose transition value is a plain value (count and integer
	 * sums carry an int8, min/max their type, bool_and/or a bool) can be
	 * split: the others serialise an internal state DuckDB cannot produce.
	 */
	if (final)
	{
		bool		ok = false;

		if (strcmp(name, "sum") == 0 && argtype == BYTEAOID &&
			gg_duckdb_is_numeric_state(&ta) && agg->aggtype == NUMERICOID)
		{
			/* the partial sums packed in the states, added up */
			memset(type, 0, sizeof(*type));
			type->typid = NUMERICOID;
			type->typmod = -1;
			type->duck = DUCKDB_TYPE_DECIMAL;
			type->width = 38;
			type->scale = ta.scale;
			appendStringInfo(out, "CAST(sum(struct_extract(%s, 's')) AS DECIMAL(38,%d))",
							 a.data, ta.scale);
			return true;
		}
		if (strcmp(name, "count") == 0 || strcmp(name, "sum") == 0)
		{
			ok = argtype == INT8OID && agg->aggtype == INT8OID;
			gg_duckdb_type_map(INT8OID, -1, type);
			cast = "BIGINT";
			name = "sum";		/* counts are combined by adding them */
		}
		else if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0)
		{
			ok = argtype == agg->aggtype && !type_is_float(argtype) ;
			*type = ta;
			cast = gg_duckdb_type_sql(type);
		}
		else if (strcmp(name, "bool_and") == 0 || strcmp(name, "bool_or") == 0)
		{
			ok = argtype == BOOLOID;
			gg_duckdb_type_map(BOOLOID, -1, type);
			cast = "BOOLEAN";
		}
		if (!ok)
			REJECT(ctx, "final %s over a %s partial", name,
				   OidIsValid(argtype) ? format_type_be(argtype) : "?");
	}
	else if (strcmp(name, "count") == 0)
	{
		gg_duckdb_type_map(INT8OID, -1, type);
		cast = "BIGINT";
	}
	else if (strcmp(name, "sum") == 0)
	{
		if (argtype == INT2OID || argtype == INT4OID)
		{
			gg_duckdb_type_map(INT8OID, -1, type);
			cast = "BIGINT";
		}
		else if (partial && argtype == NUMERICOID && ta.duck == DUCKDB_TYPE_DECIMAL)
			return deparse_numeric_state(ctx, agg, a.data, ta.scale, out, type);
		else if (partial)
			REJECT(ctx, "partial sum(%s) serialises an internal state", format_type_be(argtype));
		else if (argtype == INT8OID)
		{
			memset(type, 0, sizeof(*type));
			type->typid = NUMERICOID;
			type->typmod = -1;
			type->duck = DUCKDB_TYPE_DECIMAL;
			type->width = 38;
			type->scale = 0;
			cast = "DECIMAL(38,0)";
		}
		else if (argtype == NUMERICOID)
		{
			memset(type, 0, sizeof(*type));
			type->typid = NUMERICOID;
			type->typmod = -1;
			type->duck = DUCKDB_TYPE_DECIMAL;
			type->width = 38;
			type->scale = ta.scale;
			cast = psprintf("DECIMAL(38,%d)", ta.scale);
		}
		else
			REJECT(ctx, "sum(%s)", format_type_be(argtype));
	}
	else if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0)
	{
		if (!(type_is_integer(argtype) || type_is_float(argtype) || argtype == NUMERICOID ||
			  type_is_text(argtype) || argtype == DATEOID || argtype == TIMESTAMPOID ||
			  argtype == TIMESTAMPTZOID || argtype == INTERVALOID))
			REJECT(ctx, "%s(%s)", name, format_type_be(argtype));
		if (type_is_text(argtype) && !collation_is_binary(agg->inputcollid))
			REJECT(ctx, "%s(text) under a non-C collation", name);
		*type = ta;
		cast = gg_duckdb_type_sql(type);
	}
	else if (strcmp(name, "bool_and") == 0 || strcmp(name, "bool_or") == 0)
	{
		if (argtype != BOOLOID)
			REJECT(ctx, "%s(%s)", name, format_type_be(argtype));
		gg_duckdb_type_map(BOOLOID, -1, type);
		cast = "BOOLEAN";
	}
	else if (strcmp(name, "avg") == 0 && partial && argtype == NUMERICOID &&
			 ta.duck == DUCKDB_TYPE_DECIMAL)
		return deparse_numeric_state(ctx, agg, a.data, ta.scale, out, type);
	else
		REJECT(ctx, "aggregate %s", name);

	/* the type PostgreSQL expects from this node must be what we produce */
	if (agg->aggtype != type->typid)
		REJECT(ctx, "aggregate %s declares %s, %s generated", name,
			   format_type_be(agg->aggtype), format_type_be(type->typid));

	appendStringInfo(out, "CAST(%s(%s%s)", name, agg->aggdistinct ? "DISTINCT " : "", a.data);
	if (agg->aggfilter)
	{
		StringInfoData f;
		GGTypeInfo	tf;

		initStringInfo(&f);
		if (!deparse_expr(ctx, (Node *) agg->aggfilter, &f, &tf))
			return false;
		appendStringInfo(out, " FILTER (WHERE %s)", f.data);
	}
	appendStringInfo(out, " AS %s)", cast);
	return true;
}

static bool
deparse_expr(DeparseCtx *ctx, Node *node, StringInfo out, GGTypeInfo *type)
{
	if (node == NULL)
		REJECT(ctx, "empty expression");

	switch (nodeTag(node))
	{
		case T_Var:
			return deparse_var(ctx, (Var *) node, out, type);
		case T_Const:
			return deparse_const(ctx, (Const *) node, out, type);
		case T_OpExpr:
			return deparse_opexpr(ctx, (OpExpr *) node, out, type);
		case T_FuncExpr:
			return deparse_funcexpr(ctx, (FuncExpr *) node, out, type);
		case T_Aggref:
			return deparse_aggref(ctx, (Aggref *) node, out, type);
		case T_RelabelType:
			{
				RelabelType *r = (RelabelType *) node;
				GGTypeInfo	inner;

				if (!deparse_expr(ctx, (Node *) r->arg, out, &inner))
					return false;
				return map_type(ctx, r->resulttype, r->resulttypmod, type);
			}
		case T_BoolExpr:
			{
				BoolExpr   *b = (BoolExpr *) node;
				ListCell   *lc;
				bool		first = true;
				GGTypeInfo	t;

				gg_duckdb_type_map(BOOLOID, -1, type);
				if (b->boolop == NOT_EXPR)
				{
					appendStringInfoString(out, "(NOT ");
					if (!deparse_expr(ctx, (Node *) linitial(b->args), out, &t))
						return false;
					appendStringInfoChar(out, ')');
					return true;
				}
				appendStringInfoChar(out, '(');
				foreach(lc, b->args)
				{
					if (!first)
						appendStringInfoString(out, b->boolop == AND_EXPR ? " AND " : " OR ");
					first = false;
					if (!deparse_expr(ctx, (Node *) lfirst(lc), out, &t))
						return false;
				}
				appendStringInfoChar(out, ')');
				return true;
			}
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) node;
				GGTypeInfo	t;

				if (nt->argisrow)
					REJECT(ctx, "row IS NULL");
				gg_duckdb_type_map(BOOLOID, -1, type);
				appendStringInfoChar(out, '(');
				if (!deparse_expr(ctx, (Node *) nt->arg, out, &t))
					return false;
				appendStringInfoString(out, nt->nulltesttype == IS_NULL ? " IS NULL)" : " IS NOT NULL)");
				return true;
			}
		case T_BooleanTest:
			{
				BooleanTest *bt = (BooleanTest *) node;
				GGTypeInfo	t;
				const char *what;

				switch (bt->booltesttype)
				{
					case IS_TRUE: what = " IS TRUE)"; break;
					case IS_NOT_TRUE: what = " IS NOT TRUE)"; break;
					case IS_FALSE: what = " IS FALSE)"; break;
					case IS_NOT_FALSE: what = " IS NOT FALSE)"; break;
					case IS_UNKNOWN: what = " IS NULL)"; break;
					case IS_NOT_UNKNOWN: what = " IS NOT NULL)"; break;
					default: REJECT(ctx, "boolean test");
				}
				gg_duckdb_type_map(BOOLOID, -1, type);
				appendStringInfoChar(out, '(');
				if (!deparse_expr(ctx, (Node *) bt->arg, out, &t))
					return false;
				appendStringInfoString(out, what);
				return true;
			}
		case T_CaseExpr:
			{
				CaseExpr   *c = (CaseExpr *) node;
				ListCell   *lc;
				GGTypeInfo	t;

				if (c->arg != NULL)
					REJECT(ctx, "CASE <expr> WHEN form");
				if (!map_type(ctx, c->casetype, -1, type))
					return false;
				appendStringInfoString(out, "(CASE");
				foreach(lc, c->args)
				{
					CaseWhen   *w = (CaseWhen *) lfirst(lc);

					appendStringInfoString(out, " WHEN ");
					if (!deparse_expr(ctx, (Node *) w->expr, out, &t))
						return false;
					appendStringInfoString(out, " THEN ");
					if (!deparse_expr(ctx, (Node *) w->result, out, &t))
						return false;
				}
				if (c->defresult)
				{
					appendStringInfoString(out, " ELSE ");
					if (!deparse_expr(ctx, (Node *) c->defresult, out, &t))
						return false;
				}
				appendStringInfoString(out, " END)");
				return true;
			}
		case T_CoalesceExpr:
			{
				CoalesceExpr *c = (CoalesceExpr *) node;
				ListCell   *lc;
				bool		first = true;
				GGTypeInfo	t;

				if (!map_type(ctx, c->coalescetype, -1, type))
					return false;
				appendStringInfoString(out, "COALESCE(");
				foreach(lc, c->args)
				{
					if (!first)
						appendStringInfoString(out, ", ");
					first = false;
					if (!deparse_expr(ctx, (Node *) lfirst(lc), out, &t))
						return false;
				}
				appendStringInfoChar(out, ')');
				return true;
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *sa = (ScalarArrayOpExpr *) node;
				Node	   *arr = (Node *) lsecond(sa->args);
				char	   *opname = get_opname(sa->opno);
				GGTypeInfo	t;
				Const	   *ac;
				ArrayType  *arrval;
				Datum	   *elems;
				bool	   *nulls;
				int			nelems,
							i;
				int16		elmlen;
				bool		elmbyval;
				char		elmalign;
				Oid			elemtype;

				if (opname == NULL || !(strcmp(opname, "=") == 0 || strcmp(opname, "<>") == 0) ||
					(strcmp(opname, "=") == 0) != sa->useOr)
					REJECT(ctx, "array operator %s", opname ? opname : "?");
				if (!IsA(arr, Const) || ((Const *) arr)->constisnull)
					REJECT(ctx, "non-constant array in ANY/ALL");
				ac = (Const *) arr;
				arrval = DatumGetArrayTypeP(ac->constvalue);
				elemtype = ARR_ELEMTYPE(arrval);
				if (!type_is_comparable_family(exprType((Node *) linitial(sa->args)), elemtype))
					REJECT(ctx, "ANY/ALL over %s", format_type_be(elemtype));
				get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
				deconstruct_array(arrval, elemtype, elmlen, elmbyval, elmalign, &elems, &nulls, &nelems);
				if (nelems == 0)
					REJECT(ctx, "empty array in ANY/ALL");
				gg_duckdb_type_map(BOOLOID, -1, type);
				appendStringInfoChar(out, '(');
				if (!deparse_expr(ctx, (Node *) linitial(sa->args), out, &t))
					return false;
				appendStringInfoString(out, sa->useOr ? " IN (" : " NOT IN (");
				for (i = 0; i < nelems; i++)
				{
					Const	   *ec = makeConst(elemtype, -1, ac->constcollid, elmlen,
											   elems[i], nulls[i], elmbyval);

					if (i > 0)
						appendStringInfoString(out, ", ");
					if (!deparse_const(ctx, ec, out, &t))
						return false;
				}
				appendStringInfoString(out, "))");
				return true;
			}
		case T_NullIfExpr:
			{
				NullIfExpr *n = (NullIfExpr *) node;
				GGTypeInfo	t;

				if (!type_is_comparable_family(exprType((Node *) linitial(n->args)),
											   exprType((Node *) lsecond(n->args))))
					REJECT(ctx, "NULLIF on incomparable types");
				if (!map_type(ctx, n->opresulttype, -1, type))
					return false;
				appendStringInfoString(out, "NULLIF(");
				if (!deparse_expr(ctx, (Node *) linitial(n->args), out, &t))
					return false;
				appendStringInfoString(out, ", ");
				if (!deparse_expr(ctx, (Node *) lsecond(n->args), out, &t))
					return false;
				appendStringInfoChar(out, ')');
				return true;
			}
		case T_Param:
			REJECT(ctx, "parameter reference");
		case T_SubPlan:
		case T_AlternativeSubPlan:
			REJECT(ctx, "subplan");
		default:
			REJECT(ctx, "expression node %d", (int) nodeTag(node));
	}
}

/* ---------- plan nodes ---------- */

/*
 * A leaf: an ordinary plan node pulled through gg_leaf(i).  Its output
 * columns are its targetlist's types.
 */
/*
 * The DECIMAL shape of a numeric expression over the columns of `child`,
 * by the rules deparse_opexpr applies: Vars and Consts by their type,
 * + and - and * by DuckDB's width and scale rules.
 */
static bool
expr_decimal_shape(Node *expr, Plan *child, int *width, int *scale)
{
	GGTypeInfo	ti;

	if (expr == NULL)
		return false;
	switch (nodeTag(expr))
	{
		case T_Var:
			{
				Var		   *var = (Var *) expr;
				TargetEntry *te;

				if (var->varno != OUTER_VAR || child == NULL ||
					var->varattno < 1 || var->varattno > list_length(child->targetlist))
					return false;
				te = (TargetEntry *) list_nth(child->targetlist, var->varattno - 1);
				if (!gg_duckdb_type_map(exprType((Node *) te->expr), exprTypmod((Node *) te->expr), &ti))
					return false;
				return decimal_shape(&ti, width, scale);
			}
		case T_Const:
			{
				Const	   *c = (Const *) expr;

				if (c->consttype == NUMERICOID)
				{
					uint8		w,
								sc;
					char	   *text;

					if (c->constisnull)
						return false;
					text = DatumGetCString(DirectFunctionCall1(numeric_out, c->constvalue));
					if (strcmp(text, "NaN") == 0)
						return false;
					numeric_const_shape(text, &w, &sc);
					*width = w;
					*scale = sc;
					return true;
				}
				if (!gg_duckdb_type_map(c->consttype, c->consttypmod, &ti))
					return false;
				return decimal_shape(&ti, width, scale);
			}
		case T_RelabelType:
			return expr_decimal_shape((Node *) ((RelabelType *) expr)->arg, child, width, scale);
		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) expr;
				char	   *name;
				int			w1,
							s1,
							w2,
							s2;
				bool		ok;

				if (op->opresulttype != NUMERICOID || list_length(op->args) != 2)
					return false;
				name = get_opname(op->opno);
				if (name == NULL)
					return false;
				ok = expr_decimal_shape((Node *) linitial(op->args), child, &w1, &s1) &&
					expr_decimal_shape((Node *) lsecond(op->args), child, &w2, &s2);
				if (!ok)
					return false;
				if (strcmp(name, "*") == 0)
				{
					*width = w1 + w2;
					*scale = s1 + s2;
				}
				else if (strcmp(name, "+") == 0 || strcmp(name, "-") == 0)
				{
					*scale = Max(s1, s2);
					*width = Max(w1 - s1, w2 - s2) + *scale + 1;
				}
				else
					return false;
				return *width <= 38 && *scale <= 38;
			}
		default:
			return false;
	}
}

/*
 * Is column `col` of leaf `plan` the serialised state of a partial
 * sum(numeric) or avg(numeric) computed below it, and at what scale?  The
 * leaf is typically a Motion above the partial Agg, possibly through nodes
 * that pass the column through.
 */
static bool
numeric_state_scale(Plan *plan, int col, int *scale)
{
	int			depth;

	for (depth = 0; plan != NULL && depth < 8; depth++)
	{
		TargetEntry *te;
		Node	   *expr;

		if (col < 0 || col >= list_length(plan->targetlist))
			return false;
		te = (TargetEntry *) list_nth(plan->targetlist, col);
		expr = (Node *) te->expr;
		if (IsA(expr, RelabelType))
			expr = (Node *) ((RelabelType *) expr)->arg;

		if (IsA(plan, Agg) && IsA(expr, Aggref))
		{
			Aggref	   *agg = (Aggref *) expr;
			char	   *name;
			int			width;

			if (agg->aggsplit != AGGSPLIT_INITIAL_SERIAL || agg->aggtype != BYTEAOID ||
				list_length(agg->args) != 1 || agg->aggdistinct != NIL ||
				agg->aggorder != NIL || agg->aggdirectargs != NIL)
				return false;
			name = get_func_name(agg->aggfnoid);
			if (name == NULL || (strcmp(name, "sum") != 0 && strcmp(name, "avg") != 0) ||
				exprType((Node *) ((TargetEntry *) linitial(agg->args))->expr) != NUMERICOID)
				return false;
			return expr_decimal_shape((Node *) ((TargetEntry *) linitial(agg->args))->expr,
									  plan->lefttree, &width, scale);
		}
		if (!IsA(expr, Var) || ((Var *) expr)->varno != OUTER_VAR)
			return false;
		col = ((Var *) expr)->varattno - 1;
		plan = plan->lefttree;
	}
	return false;
}

/*
 * The DuckDB shape of column `col` of a leaf.  The coordinator (deparser)
 * and the segments (gg_leaf's bind) must agree, so both come here.
 */
bool
gg_duckdb_leaf_column_type(Plan *plan, int col, GGTypeInfo *ti)
{
	TargetEntry *te;
	Oid			typid;
	int			scale;

	if (col < 0 || col >= list_length(plan->targetlist))
		return false;
	te = (TargetEntry *) list_nth(plan->targetlist, col);
	typid = exprType((Node *) te->expr);
	if (typid == BYTEAOID && numeric_state_scale(plan, col, &scale))
	{
		gg_duckdb_numeric_state_type(ti, scale);
		return true;
	}
	return gg_duckdb_type_map(typid, exprTypmod((Node *) te->expr), ti);
}

static bool
deparse_leaf(DeparseCtx *ctx, Plan *plan, StringInfo out, NodeCols *cols)
{
	ListCell   *lc;
	int			k = 0;
	int			leafno = list_length(ctx->spec->leaves);

	cols->ncols = list_length(plan->targetlist);
	cols->types = palloc0(sizeof(GGTypeInfo) * Max(cols->ncols, 1));
	foreach(lc, plan->targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);

		if (!gg_duckdb_leaf_column_type(plan, k, &cols->types[k]))
			REJECT(ctx, "type %s is not carried",
				   format_type_with_typemod(exprType((Node *) te->expr),
											exprTypmod((Node *) te->expr)));
		k++;
	}
	ctx->spec->leaves = lappend(ctx->spec->leaves, plan);
	ctx->spec->rows_in += plan->plan_rows;
	ctx->spec->bytes_in += plan->plan_rows * Max(plan->plan_width, 1);
	appendStringInfo(out, "SELECT * FROM %s(%d)", GG_DUCKDB_LEAF_FUNCTION, leafno);
	return true;
}

/*
 * SELECT <tlist> FROM <from_clause> [WHERE ...] [GROUP BY ...] [HAVING ...]
 *
 * Vars of the node's expressions resolve through `outer`/`inner` and their
 * aliases, which `from_clause` must have introduced.
 */
static bool
deparse_projection(DeparseCtx *ctx, Plan *plan, const char *from_clause,
				   NodeCols *outer, const char *outer_alias,
				   NodeCols *inner, const char *inner_alias,
				   const char *group_by, bool is_agg, StringInfo out, NodeCols *cols)
{
	NodeCols   *saved_child = ctx->child;
	NodeCols   *saved_inner = ctx->inner;
	const char *saved_oa = ctx->outer_alias;
	const char *saved_ia = ctx->inner_alias;
	bool		saved_in_agg = ctx->in_agg;
	ListCell   *lc;
	int			k = 0;
	bool		ok = true;

	ctx->child = outer;
	ctx->outer_alias = outer_alias;
	ctx->inner = inner;
	ctx->inner_alias = inner_alias;
	ctx->in_agg = is_agg;
	cols->ncols = list_length(plan->targetlist);
	cols->types = palloc0(sizeof(GGTypeInfo) * Max(cols->ncols, 1));

	appendStringInfoString(out, "SELECT ");
	if (cols->ncols == 0)
		appendStringInfoString(out, "1 AS c1");
	foreach(lc, plan->targetlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);

		if (k > 0)
			appendStringInfoString(out, ", ");
		if (!deparse_expr(ctx, (Node *) te->expr, out, &cols->types[k]))
		{
			ok = false;
			break;
		}
		appendStringInfo(out, " AS c%d", k + 1);
		k++;
	}
	if (ok)
	{
		appendStringInfo(out, " FROM %s", from_clause);
		if (!is_agg && plan->qual != NIL)
		{
			ListCell   *qc;
			bool		first = true;
			GGTypeInfo	t;

			appendStringInfoString(out, " WHERE ");
			foreach(qc, plan->qual)
			{
				if (!first)
					appendStringInfoString(out, " AND ");
				first = false;
				if (!deparse_expr(ctx, (Node *) lfirst(qc), out, &t))
				{
					ok = false;
					break;
				}
			}
		}
		if (ok && group_by != NULL)
			appendStringInfoString(out, group_by);
		if (ok && is_agg && plan->qual != NIL)
		{
			ListCell   *qc;
			bool		first = true;
			GGTypeInfo	t;

			appendStringInfoString(out, " HAVING ");
			foreach(qc, plan->qual)
			{
				if (!first)
					appendStringInfoString(out, " AND ");
				first = false;
				if (!deparse_expr(ctx, (Node *) lfirst(qc), out, &t))
				{
					ok = false;
					break;
				}
			}
		}
	}
	ctx->child = saved_child;
	ctx->inner = saved_inner;
	ctx->outer_alias = saved_oa;
	ctx->inner_alias = saved_ia;
	ctx->in_agg = saved_in_agg;
	return ok;
}

/* "(<sql>) AS n<k>" with a fresh alias; the alias is returned through *alias. */
static char *
subquery_from(DeparseCtx *ctx, const char *sql, char **alias)
{
	*alias = psprintf("n%d", ctx->next_alias++);
	return psprintf("(%s) AS %s", sql, *alias);
}

/*
 * PostgreSQL-unit cost of an interior operator, from its input and output
 * rows the way costsize.c charges it; the gate compares it with DuckDB's.
 */
static void
add_op_cost(DeparseCtx *ctx, double cost)
{
	ctx->spec->op_cost += cost;
}

static double
rows_of(Plan *plan)
{
	return Max(plan->plan_rows, 1.0);
}

/*
 * As cost_sort: comparisons of an in-memory sort, or of a bounded (top-N)
 * one when a LIMIT above it caps the output.
 */
static double
sort_cost(double rows, double limit_rows)
{
	double		n = Max(rows, 2.0);
	double		l = limit_rows > 0 && limit_rows < n ? Max(2.0 * limit_rows, 2.0) : n;

	return 2.0 * cpu_operator_cost * n * log(l) / log(2.0);
}

static bool
count_aggrefs_walker(Node *node, int *count)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		(*count)++;
		return false;
	}
	return expression_tree_walker(node, count_aggrefs_walker, count);
}

/* The aggregates an Agg node evaluates, as cost_agg counts them. */
static int
count_aggrefs(Plan *plan)
{
	int			count = 0;

	count_aggrefs_walker((Node *) plan->targetlist, &count);
	count_aggrefs_walker((Node *) plan->qual, &count);
	return Max(count, 1);
}

static bool partition_selector_hazard(Plan *outer, Plan *inner);

/*
 * Why a join must stay a leaf, or NULL when DuckDB can run it.
 */
static const char *
join_leaf_reason(Plan *plan)
{
	Join	   *join = (Join *) plan;

	if (plan->initPlan != NIL)
		return "join with an InitPlan";
	if (plan->lefttree == NULL || plan->righttree == NULL)
		return "join without two inputs";
	/* the executor keeps the build-side-first order such a join needs */
	if (partition_selector_hazard(plan->lefttree, plan->righttree))
		return "a PartitionSelector on the inner side prunes the outer side";
	switch (join->jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_RIGHT:
		case JOIN_FULL:
		case JOIN_SEMI:
		case JOIN_ANTI:
			break;
		case JOIN_LASJ_NOTIN:
			return "NOT IN join";
		default:
			return "join type with deduplication";
	}
	if (IsA(plan, NestLoop))
	{
		NestLoop   *nl = (NestLoop *) plan;

		if (nl->nestParams != NIL)
			return "nested loop with parameters";
		if (nl->shared_outer || nl->singleton_outer)
			return "nested loop with a shared or singleton outer side";
		return NULL;
	}
	if (IsA(plan, HashJoin))
	{
		HashJoin   *hj = (HashJoin *) plan;

		/*
		 * Both optimizers leave hashqualclauses empty for an equality join;
		 * only IS NOT DISTINCT FROM joins fill it (nodeHashjoin.c).
		 */
		if (hj->hashqualclauses != NIL)
			return "IS NOT DISTINCT FROM join";
		if (!IsA(plan->righttree, Hash))
			return "hash join without a Hash input";
		return NULL;
	}
	return IsA(plan, MergeJoin) ? NULL : "unknown join node";
}

static bool
join_is_interior(Plan *plan)
{
	return join_leaf_reason(plan) == NULL;
}

static bool
node_is_interior_base(Plan *plan)
{
	switch (nodeTag(plan))
	{
		case T_HashJoin:
		case T_MergeJoin:
		case T_NestLoop:
			return join_is_interior(plan);
		case T_Append:
			{
				Append	   *ap = (Append *) plan;

				return plan->initPlan == NIL && ap->appendplans != NIL &&
					ap->part_prune_info == NULL && ap->join_prune_paramids == NIL;
			}
		case T_Result:
			{
				Result	   *r = (Result *) plan;

				return plan->lefttree != NULL && r->numHashFilterCols == 0 &&
					plan->initPlan == NIL;
			}
		case T_Agg:
			{
				Agg		   *agg = (Agg *) plan;

				if (plan->lefttree == NULL || plan->initPlan != NIL ||
					(agg->aggsplit != AGGSPLIT_SIMPLE &&
					 agg->aggsplit != AGGSPLIT_INITIAL_SERIAL &&
					 agg->aggsplit != AGGSPLIT_FINAL_DESERIAL) ||
					agg->groupingSets != NIL ||
					agg->chain != NIL || agg->aggParams != NULL)
					return false;
				if (agg->aggstrategy == AGG_PLAIN || agg->aggstrategy == AGG_HASHED)
					return true;
				/* a sorted Agg over the Sort that groups it: DuckDB hashes instead */
				return agg->aggstrategy == AGG_SORTED &&
					(agg->numCols == 0 ||
					 (IsA(plan->lefttree, Sort) && plan->lefttree->initPlan == NIL));
			}
		case T_Material:
			{
				Material   *m = (Material *) plan;

				return plan->lefttree != NULL && plan->initPlan == NIL &&
					!m->cdb_strict && !m->cdb_shield_child_from_rescans;
			}
		default:
			return false;
	}
}

/*
 * A sorted Agg's output comes out in the order of the Sort below it (its
 * group keys, possibly more).  DuckDB hashes instead, so the order is
 * restored with an ORDER BY over the Agg's output columns when every sort
 * key is among them; otherwise the region is simply unordered.
 */
static bool
sorted_agg_order(DeparseCtx *ctx, Agg *agg, Sort *sort, NodeCols *cols)
{
	StringInfoData ob;
	int			i;

	if (ctx->spec->agg_order != NULL)
		return true;			/* only the outermost sorted Agg matters */
	initStringInfo(&ob);
	appendStringInfoString(&ob, " ORDER BY ");
	for (i = 0; i < sort->numCols; i++)
	{
		int			incol = sort->sortColIdx[i];
		int			outcol = 0;
		ListCell   *lc;
		Oid			opfamily,
					opcintype;
		int16		strategy;
		int			k = 1;

		foreach(lc, ((Plan *) agg)->targetlist)
		{
			Expr	   *e = ((TargetEntry *) lfirst(lc))->expr;

			if (IsA(e, Var) && ((Var *) e)->varno == OUTER_VAR &&
				((Var *) e)->varattno == incol)
			{
				outcol = k;
				break;
			}
			k++;
		}
		if (outcol == 0)
			return true;		/* the key is not projected: unordered */
		if (!get_ordering_op_properties(sort->sortOperators[i], &opfamily, &opcintype, &strategy) ||
			(strategy != BTLessStrategyNumber && strategy != BTGreaterStrategyNumber))
			return true;
		if (type_is_text(cols->types[outcol - 1].typid) && !collation_is_binary(sort->collations[i]))
			return true;
		appendStringInfo(&ob, "%sc%d %s %s", i > 0 ? ", " : "", outcol,
						 strategy == BTLessStrategyNumber ? "ASC" : "DESC",
						 sort->nullsFirst[i] ? "NULLS FIRST" : "NULLS LAST");
	}
	ctx->spec->agg_order = ob.data;
	return true;
}

/*
 * A PartitionSelector on the inner side of a join feeds the dynamic scans on
 * its outer side through a parameter, which is only complete once the inner
 * side is exhausted.  DuckDB gives no build-side-first guarantee, so such a
 * join is never interior: it stays a leaf run by the executor.
 */
typedef struct PruneCtx
{
	plan_tree_base_prefix base;
	List	   *paramids;
	bool		hit;
} PruneCtx;

static bool
collect_selector_params(Node *node, PruneCtx *pctx)
{
	if (node == NULL)
		return false;
	if (IsA(node, PartitionSelector))
		pctx->paramids = lappend_int(pctx->paramids, ((PartitionSelector *) node)->paramid);
	return plan_tree_walker(node, collect_selector_params, pctx, false);
}

static bool
uses_selector_params(Node *node, PruneCtx *pctx)
{
	List	   *ids = NIL;
	ListCell   *lc;

	if (node == NULL)
		return false;
	switch (nodeTag(node))
	{
		case T_DynamicSeqScan:
			ids = ((DynamicSeqScan *) node)->join_prune_paramids;
			break;
		case T_DynamicIndexScan:
			ids = ((DynamicIndexScan *) node)->join_prune_paramids;
			break;
		case T_DynamicIndexOnlyScan:
			ids = ((DynamicIndexOnlyScan *) node)->join_prune_paramids;
			break;
		case T_DynamicBitmapHeapScan:
			ids = ((DynamicBitmapHeapScan *) node)->join_prune_paramids;
			break;
		case T_DynamicForeignScan:
			ids = ((DynamicForeignScan *) node)->join_prune_paramids;
			break;
		case T_Append:
			ids = ((Append *) node)->join_prune_paramids;
			break;
		case T_MergeAppend:
			ids = ((MergeAppend *) node)->join_prune_paramids;
			break;
		default:
			break;
	}
	foreach(lc, ids)
	{
		if (list_member_int(pctx->paramids, lfirst_int(lc)))
		{
			pctx->hit = true;
			return true;
		}
	}
	return plan_tree_walker(node, uses_selector_params, pctx, false);
}

static bool
partition_selector_hazard(Plan *outer, Plan *inner)
{
	PruneCtx	pctx;

	memset(&pctx, 0, sizeof(pctx));
	collect_selector_params((Node *) inner, &pctx);
	if (pctx.paramids == NIL)
		return false;
	uses_selector_params((Node *) outer, &pctx);
	return pctx.hit;
}

static bool deparse_join(DeparseCtx *ctx, Plan *plan, StringInfo out, NodeCols *cols);
static bool deparse_append(DeparseCtx *ctx, Append *ap, StringInfo out, NodeCols *cols);

static bool
deparse_base(DeparseCtx *ctx, Plan *plan, StringInfo out, NodeCols *cols)
{
	if (!node_is_interior_base(plan))
		return deparse_leaf(ctx, plan, out, cols);

	if (IsA(plan, HashJoin) || IsA(plan, MergeJoin) || IsA(plan, NestLoop))
		return deparse_join(ctx, plan, out, cols);
	if (IsA(plan, Append))
		return deparse_append(ctx, (Append *) plan, out, cols);

	if (IsA(plan, Material))
		return deparse_base(ctx, plan->lefttree, out, cols);

	{
		StringInfoData child_sql;
		NodeCols	childcols;
		Plan	   *child = plan->lefttree;
		char	   *from;
		char	   *alias;

		/* the Sort that groups a sorted Agg is not needed: DuckDB hashes */
		if (IsA(plan, Agg) && ((Agg *) plan)->aggstrategy == AGG_SORTED &&
			((Agg *) plan)->numCols > 0)
		{
			Sort	   *sort = (Sort *) child;
			Agg		   *agg = (Agg *) plan;
			int			i;

			for (i = 0; i < agg->numCols; i++)
			{
				int			j;
				bool		found = false;

				for (j = 0; j < sort->numCols; j++)
					if (sort->sortColIdx[j] == agg->grpColIdx[i])
						found = true;
				if (!found)
					REJECT(ctx, "sorted Agg whose Sort does not cover its group keys");
			}
			if (list_length(((Plan *) sort)->targetlist) != list_length(child->lefttree->targetlist))
				REJECT(ctx, "grouping Sort changes the column list");
			child = child->lefttree;
		}

		initStringInfo(&child_sql);
		if (!deparse_base(ctx, child, &child_sql, &childcols))
			return false;

		if (IsA(plan, Result))
		{
			Result	   *r = (Result *) plan;

			if (r->resconstantqual != NULL)
			{
				/* a one-time filter is a constant condition: fold it into WHERE */
				StringInfoData cq;
				GGTypeInfo	t;
				List	   *saved_qual = plan->qual;
				bool		ok;

				initStringInfo(&cq);
				ctx->child = &childcols;
				ctx->outer_alias = NULL;
				ok = deparse_expr(ctx, r->resconstantqual, &cq, &t);
				ctx->child = NULL;
				if (!ok)
					return false;
				plan->qual = lappend(list_copy(plan->qual), r->resconstantqual);
				from = subquery_from(ctx, child_sql.data, &alias);
				ok = deparse_projection(ctx, plan, from, &childcols, alias, NULL, NULL, NULL, false, out, cols);
				plan->qual = saved_qual;
				ctx->spec->ninterior++;
				add_op_cost(ctx, cpu_tuple_cost * rows_of(child));
				return ok;
			}
			ctx->spec->ninterior++;
			add_op_cost(ctx, cpu_tuple_cost * rows_of(child));
			from = subquery_from(ctx, child_sql.data, &alias);
			return deparse_projection(ctx, plan, from, &childcols, alias, NULL, NULL, NULL, false, out, cols);
		}
		else
		{
			Agg		   *agg = (Agg *) plan;
			StringInfoData gb;
			int			i;

			from = subquery_from(ctx, child_sql.data, &alias);
			initStringInfo(&gb);
			if (agg->numCols > 0)
			{
				appendStringInfoString(&gb, " GROUP BY ");
				for (i = 0; i < agg->numCols; i++)
				{
					int			col = agg->grpColIdx[i];

					if (col < 1 || col > childcols.ncols)
						REJECT(ctx, "grouping column out of range");
					if (type_is_float(childcols.types[col - 1].typid))
						REJECT(ctx, "grouping by a float");
					appendStringInfo(&gb, "%s%s.c%d", i > 0 ? ", " : "", alias, col);
				}
			}
			ctx->spec->ninterior++;
			ctx->spec->naggs++;
			/* as cost_agg: one operator per input row per aggregate and group key */
			add_op_cost(ctx, cpu_operator_cost * rows_of(child) *
						(count_aggrefs(plan) + agg->numCols) +
						cpu_tuple_cost * rows_of(plan) +
						(agg->aggstrategy == AGG_SORTED && agg->numCols > 0 ?
						 sort_cost(rows_of(child), 0) : 0.0));
			if (!deparse_projection(ctx, plan, from, &childcols, alias, NULL, NULL,
									agg->numCols > 0 ? gb.data : NULL, true, out, cols))
				return false;
			if (agg->aggstrategy == AGG_SORTED && agg->numCols > 0)
				return sorted_agg_order(ctx, agg, (Sort *) plan->lefttree, cols);
			return true;
		}
	}
}

/*
 * A join: SELECT <tlist> FROM (<outer>) AS a <kind> JOIN (<inner>) AS b
 * ON <clauses> [WHERE <plan.qual>].  DuckDB picks its own join order and
 * build side; the Sort children PostgreSQL added for a merge join and the
 * Hash node of a hash join are not needed.
 */
static bool
deparse_join(DeparseCtx *ctx, Plan *plan, StringInfo out, NodeCols *cols)
{
	Join	   *join = (Join *) plan;
	Plan	   *outerp = plan->lefttree;
	Plan	   *innerp = plan->righttree;
	StringInfoData osql,
				isql,
				cond,
				from;
	NodeCols	ocols,
				icols;
	char	   *oalias,
			   *ialias;
	List	   *clauses = NIL;
	ListCell   *lc;
	const char *kind;
	NodeCols   *saved_child = ctx->child;
	NodeCols   *saved_inner = ctx->inner;
	const char *saved_oa = ctx->outer_alias;
	const char *saved_ia = ctx->inner_alias;
	bool		ok = true;

	if (IsA(innerp, Hash))
		innerp = innerp->lefttree;
	if (IsA(outerp, Sort) && outerp->initPlan == NIL)
		outerp = outerp->lefttree;
	if (IsA(innerp, Sort) && innerp->initPlan == NIL)
		innerp = innerp->lefttree;

	initStringInfo(&osql);
	initStringInfo(&isql);
	if (!deparse_base(ctx, outerp, &osql, &ocols) ||
		!deparse_base(ctx, innerp, &isql, &icols))
		return false;

	switch (join->jointype)
	{
		case JOIN_INNER: kind = "JOIN"; break;
		case JOIN_LEFT: kind = "LEFT JOIN"; break;
		case JOIN_RIGHT: kind = "RIGHT JOIN"; break;
		case JOIN_FULL: kind = "FULL OUTER JOIN"; break;
		case JOIN_SEMI: kind = "SEMI JOIN"; break;
		case JOIN_ANTI: kind = "ANTI JOIN"; break;
		default: REJECT(ctx, "join type %d", (int) join->jointype);
	}

	if (IsA(plan, HashJoin))
		clauses = list_copy(((HashJoin *) plan)->hashclauses);
	else if (IsA(plan, MergeJoin))
		clauses = list_copy(((MergeJoin *) plan)->mergeclauses);
	clauses = list_concat(clauses, list_copy(join->joinqual));

	initStringInfo(&from);
	appendStringInfoString(&from, subquery_from(ctx, osql.data, &oalias));
	appendStringInfo(&from, " %s ", kind);
	appendStringInfoString(&from, subquery_from(ctx, isql.data, &ialias));

	ctx->child = &ocols;
	ctx->outer_alias = oalias;
	ctx->inner = &icols;
	ctx->inner_alias = ialias;
	initStringInfo(&cond);
	if (clauses == NIL)
		appendStringInfoString(&cond, "TRUE");
	foreach(lc, clauses)
	{
		GGTypeInfo	t;

		if (lc != list_head(clauses))
			appendStringInfoString(&cond, " AND ");
		if (!deparse_expr(ctx, (Node *) lfirst(lc), &cond, &t))
		{
			ok = false;
			break;
		}
	}
	ctx->child = saved_child;
	ctx->inner = saved_inner;
	ctx->outer_alias = saved_oa;
	ctx->inner_alias = saved_ia;
	if (!ok)
		return false;
	appendStringInfo(&from, " ON %s", cond.data);

	ctx->spec->ninterior++;
	ctx->spec->njoins++;
	{
		double		orows = rows_of(outerp);
		double		irows = rows_of(innerp);
		double		nclauses = Max(list_length(clauses), 1);

		if (IsA(plan, NestLoop))
			add_op_cost(ctx, cpu_operator_cost * orows * irows * nclauses);
		else if (IsA(plan, MergeJoin))
			add_op_cost(ctx, cpu_operator_cost * (orows + irows) * nclauses +
						(IsA(plan->lefttree, Sort) ? sort_cost(orows, 0) : 0.0) +
						(IsA(plan->righttree, Sort) ? sort_cost(irows, 0) : 0.0));
		else
			add_op_cost(ctx, cpu_operator_cost * (orows + irows) * nclauses +
						cpu_tuple_cost * irows);
		add_op_cost(ctx, cpu_tuple_cost * rows_of(plan));
	}
	return deparse_projection(ctx, plan, from.data, &ocols, oalias, &icols, ialias,
							  NULL, false, out, cols);
}

/*
 * Append: SELECT <tlist> FROM ((<child1>) UNION ALL (<child2>) ...) AS n<k>.
 * Every child exposes the same columns in the same order.
 */
static bool
deparse_append(DeparseCtx *ctx, Append *ap, StringInfo out, NodeCols *cols)
{
	StringInfoData u;
	NodeCols	first;
	ListCell   *lc;
	char	   *alias;
	char	   *from;
	bool		have_first = false;

	initStringInfo(&u);
	foreach(lc, ap->appendplans)
	{
		Plan	   *child = (Plan *) lfirst(lc);
		StringInfoData csql;
		NodeCols	ccols;

		initStringInfo(&csql);
		if (!deparse_base(ctx, child, &csql, &ccols))
			return false;
		if (have_first)
		{
			int			i;

			if (ccols.ncols != first.ncols)
				REJECT(ctx, "Append children with different column counts");
			for (i = 0; i < ccols.ncols; i++)
				if (ccols.types[i].duck != first.types[i].duck ||
					ccols.types[i].width != first.types[i].width ||
					ccols.types[i].scale != first.types[i].scale)
					REJECT(ctx, "Append children with different column types");
			appendStringInfoString(&u, " UNION ALL ");
		}
		else
		{
			first = ccols;
			have_first = true;
		}
		appendStringInfo(&u, "(%s)", csql.data);
	}
	ctx->spec->ninterior++;
	add_op_cost(ctx, cpu_tuple_cost * 0.5 * rows_of((Plan *) ap));
	from = subquery_from(ctx, u.data, &alias);
	return deparse_projection(ctx, (Plan *) ap, from, &first, alias, NULL, NULL,
							  NULL, false, out, cols);
}

/*
 * Sort keys of a Sort node as an ORDER BY clause over columns c<k>.
 */
static bool
deparse_order_by(DeparseCtx *ctx, Sort *sort, NodeCols *cols, StringInfo out)
{
	int			i;

	appendStringInfoString(out, " ORDER BY ");
	for (i = 0; i < sort->numCols; i++)
	{
		int			col = sort->sortColIdx[i];
		Oid			opfamily,
					opcintype;
		int16		strategy;
		GGTypeInfo *t;

		if (col < 1 || col > cols->ncols)
			REJECT(ctx, "sort column out of range");
		t = &cols->types[col - 1];
		if (!get_ordering_op_properties(sort->sortOperators[i], &opfamily, &opcintype, &strategy))
			REJECT(ctx, "sort operator %u", sort->sortOperators[i]);
		if (strategy != BTLessStrategyNumber && strategy != BTGreaterStrategyNumber)
			REJECT(ctx, "sort strategy %d", (int) strategy);
		if (type_is_text(t->typid) && !collation_is_binary(sort->collations[i]))
			REJECT(ctx, "sorting text under a non-C collation");
		if (t->typid == BYTEAOID)
			REJECT(ctx, "sorting bytea");
		appendStringInfo(out, "%sc%d %s %s", i > 0 ? ", " : "", col,
						 strategy == BTLessStrategyNumber ? "ASC" : "DESC",
						 sort->nullsFirst[i] ? "NULLS FIRST" : "NULLS LAST");
	}
	return true;
}

static bool
limit_value(DeparseCtx *ctx, Node *expr, int64 *value, bool *present)
{
	Const	   *c;

	*present = false;
	if (expr == NULL)
		return true;
	if (!IsA(expr, Const) || ((Const *) expr)->constisnull)
	{
		if (IsA(expr, Const))
			return true;		/* NULL means no limit */
		REJECT(ctx, "non-constant LIMIT/OFFSET");
	}
	c = (Const *) expr;
	if (c->consttype != INT8OID)
		REJECT(ctx, "LIMIT/OFFSET of type %s", format_type_be(c->consttype));
	*value = DatumGetInt64(c->constvalue);
	if (*value < 0)
		REJECT(ctx, "negative LIMIT/OFFSET");
	*present = true;
	return true;
}

/*
 * Deparse the region rooted at `root`.  On success spec->sql, leaves,
 * params, outtypes, flags, label, rows_in and the operator counts are
 * filled in; on failure spec->reject says why.
 */
bool
gg_duckdb_deparse_region(PlannedStmt *stmt, Plan *root, GGRegionSpec *spec)
{
	DeparseCtx	ctx;
	Plan	   *node = root;
	Limit	   *limit = NULL;
	Unique	   *uniq = NULL;
	Sort	   *sort = NULL;
	StringInfoData base_sql;
	StringInfoData sql;
	NodeCols	cols;
	int			i;

	memset(&ctx, 0, sizeof(ctx));
	ctx.spec = spec;
	ctx.next_alias = 1;
	spec->reject = NULL;
	spec->leaves = NIL;
	spec->params = NIL;
	spec->rows_in = 0;
	spec->ninterior = 0;
	spec->naggs = 0;
	spec->nsorts = 0;
	spec->njoins = 0;
	spec->agg_order = NULL;
	spec->ordered = false;

	/* the top chain: [Limit] [Unique] [Sort], Materials in between are transparent */
	for (;;)
	{
		if (IsA(node, Material) && node_is_interior_base(node))
		{
			node = node->lefttree;
			continue;
		}
		if (IsA(node, Limit) && limit == NULL && uniq == NULL && sort == NULL &&
			node->initPlan == NIL && node->qual == NIL)
		{
			limit = (Limit *) node;
			node = node->lefttree;
			continue;
		}
		if (IsA(node, Unique) && uniq == NULL && sort == NULL && node->initPlan == NIL)
		{
			uniq = (Unique *) node;
			if (uniq->numCols != list_length(node->targetlist))
				REJECT(&ctx, "Unique over a subset of the columns");
			node = node->lefttree;
			continue;
		}
		if (IsA(node, Sort) && sort == NULL && node->initPlan == NIL)
		{
			sort = (Sort *) node;
			node = node->lefttree;
			continue;
		}
		break;
	}
	if (node == NULL)
		REJECT(&ctx, "empty subtree");

	/* every node of the chain must merely pass columns through */
	if ((limit && list_length(((Plan *) limit)->targetlist) != list_length(node->targetlist)) ||
		(uniq && list_length(((Plan *) uniq)->targetlist) != list_length(node->targetlist)) ||
		(sort && list_length(((Plan *) sort)->targetlist) != list_length(node->targetlist)))
		REJECT(&ctx, "top chain node changes the column list");

	initStringInfo(&base_sql);
	if (!deparse_base(&ctx, node, &base_sql, &cols))
		return false;
	if (limit == NULL && uniq == NULL && sort == NULL && spec->ninterior == 0)
	{
		if (IsA(node, HashJoin) || IsA(node, MergeJoin) || IsA(node, NestLoop))
			REJECT(&ctx, "%s", join_leaf_reason(node));
		REJECT(&ctx, "nothing for DuckDB to do");
	}

	initStringInfo(&sql);
	appendStringInfo(&sql, "SELECT %s* FROM (%s) AS n%d", uniq ? "DISTINCT " : "",
					 base_sql.data, ctx.next_alias++);
	if (sort)
	{
		if (!deparse_order_by(&ctx, sort, &cols, &sql))
			return false;
		spec->nsorts++;
		spec->ninterior++;
		spec->ordered = true;
		{
			double		bound = 0;
			int64		count = 0,
						offset = 0;
			bool		has_count = false,
						has_offset = false;

			if (limit != NULL && uniq == NULL &&
				limit_value(&ctx, limit->limitCount, &count, &has_count) &&
				limit_value(&ctx, limit->limitOffset, &offset, &has_offset) && has_count)
				bound = (double) count + (has_offset ? (double) offset : 0);
			else if (limit == NULL && uniq == NULL && spec->parent_limit > 0)
				bound = spec->parent_limit;		/* the executor's sort is bounded by the Limit above */
			add_op_cost(&ctx, sort_cost(rows_of(node), bound));
		}
	}
	else if (spec->agg_order != NULL && node_is_interior_base(node) && IsA(node, Agg))
	{
		appendStringInfoString(&sql, spec->agg_order);
		spec->ordered = true;
	}
	if (uniq)
	{
		spec->ninterior++;
		add_op_cost(&ctx, cpu_operator_cost * rows_of(node) * Max(uniq->numCols, 1));
	}
	if (limit)
	{
		int64		count = 0,
					offset = 0;
		bool		has_count,
					has_offset;

		if (!limit_value(&ctx, limit->limitCount, &count, &has_count) ||
			!limit_value(&ctx, limit->limitOffset, &offset, &has_offset))
			return false;
		if (has_count)
			appendStringInfo(&sql, " LIMIT " INT64_FORMAT, count);
		if (has_offset)
			appendStringInfo(&sql, " OFFSET " INT64_FORMAT, offset);
		spec->ninterior++;
	}

	spec->sql = sql.data;
	spec->rows_out = rows_of(root);
	spec->bytes_out = spec->rows_out * Max(root->plan_width, 1);
	spec->nleaves = list_length(spec->leaves);
	spec->ncols = cols.ncols;
	spec->outtypes = cols.types;
	spec->flags = (spec->naggs > 0 || sort != NULL || uniq != NULL) ?
		(CUSTOMSCAN_GP_MEMORY_INTENSIVE | CUSTOMSCAN_GP_BLOCKING) : 0;
	{
		StringInfoData label;

		initStringInfo(&label);
		if (limit)
			appendStringInfoString(&label, "Limit ");
		if (uniq)
			appendStringInfoString(&label, "Unique ");
		if (sort)
			appendStringInfoString(&label, "Sort ");
		for (i = 0; i < spec->njoins; i++)
			appendStringInfoString(&label, "Join ");
		for (i = 0; i < spec->naggs; i++)
			appendStringInfoString(&label, "Agg ");
		appendStringInfo(&label, "over %d %s", spec->nleaves, spec->nleaves == 1 ? "leaf" : "leaves");
		spec->label = label.data;
	}
	return true;
}
