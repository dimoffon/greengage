/*-------------------------------------------------------------------------
 *
 * fdw.c
 *	  The gg_duckdb foreign data wrapper: Parquet, CSV and JSON files read
 *	  by the embedded DuckDB, sharded over the segments by file.
 *
 * A foreign table names its files (a path, a glob or a comma-separated
 * list of them) and executes on all segments by default; each segment
 * keeps the files whose path hash falls to it, so every file is read by
 * exactly one segment for any segment count.  The scan runs on the shared
 * DuckDB query executor (query.c) with the deparsable quals pushed into
 * DuckDB, and the planner pass inlines such a scan into a region as a
 * native reader, so joins and aggregates above it see no row conversion on
 * the way in.  DuckDB may only touch files under gg_duckdb.data_directories.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "cdb/cdbpathlocus.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/value.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/syscache.h"

#include "gg_duckdb/gg_duckdb.h"

PG_FUNCTION_INFO_V1(gg_duckdb_fdw_handler);
PG_FUNCTION_INFO_V1(gg_duckdb_fdw_validator);
PG_FUNCTION_INFO_V1(gg_duckdb_foreign_files);

#define GG_FDW_PRIVATE_VERSION	1

static char *duck_literal(const char *s);
static char *duck_ident(const char *s);
static List *duck_text_column(const char *sql, const char *what);

/* ---------- options ---------- */

typedef struct GGOptionDef
{
	const char *name;
	Oid			context;
} GGOptionDef;

static const GGOptionDef valid_options[] =
{
	{"format", ForeignDataWrapperRelationId},
	{"format", ForeignServerRelationId},
	{"format", ForeignTableRelationId},
	/* S3-compatible stores: the endpoint on the server, the keys on the user mapping */
	{"s3_endpoint", ForeignServerRelationId},
	{"s3_region", ForeignServerRelationId},
	{"s3_url_style", ForeignServerRelationId},
	{"s3_use_ssl", ForeignServerRelationId},
	{"s3_access_key_id", UserMappingRelationId},
	{"s3_secret_access_key", UserMappingRelationId},
	{"s3_session_token", UserMappingRelationId},
	/* an Iceberg REST catalog: tables named namespace.table in `location` */
	{"iceberg_endpoint", ForeignServerRelationId},
	{"iceberg_warehouse", ForeignServerRelationId},
	{"iceberg_auth", ForeignServerRelationId},
	{"iceberg_oauth2_server_uri", ForeignServerRelationId},
	{"iceberg_token", UserMappingRelationId},
	{"iceberg_client_id", UserMappingRelationId},
	{"iceberg_client_secret", UserMappingRelationId},
	{"location", ForeignTableRelationId},
	{"hive_partitioning", ForeignServerRelationId},
	{"hive_partitioning", ForeignTableRelationId},
	{"union_by_name", ForeignServerRelationId},
	{"union_by_name", ForeignTableRelationId},
	/* JSON: auto, newline_delimited, array or unstructured */
	{"json_format", ForeignServerRelationId},
	{"json_format", ForeignTableRelationId},
	/* CSV and JSON reader options, passed through */
	{"header", ForeignTableRelationId},
	{"delim", ForeignTableRelationId},
	{"quote", ForeignTableRelationId},
	{"escape", ForeignTableRelationId},
	{"nullstr", ForeignTableRelationId},
	{"skip", ForeignTableRelationId},
	{"dateformat", ForeignTableRelationId},
	{"timestampformat", ForeignTableRelationId},
	{"compression", ForeignTableRelationId},
	{"sample_size", ForeignTableRelationId},
	{"maximum_object_size", ForeignTableRelationId},
	{"column_name", AttributeRelationId},
	/* the core's own: which columns distribute inserted rows over the segments */
	{"insert_dist_by_key", AttributeRelationId},
	{"insert_dist_by_key_weight", AttributeRelationId},
	{NULL, InvalidOid}
};

static const char *const reader_option_names[] = {
	"header", "delim", "quote", "escape", "nullstr", "skip", "dateformat",
	"timestampformat", "compression", "sample_size", "maximum_object_size", NULL
};

static bool
is_reader_option(const char *name)
{
	int			i;

	for (i = 0; reader_option_names[i]; i++)
		if (strcmp(reader_option_names[i], name) == 0)
			return true;
	return false;
}

static bool
format_is_known(const char *format)
{
	return strcmp(format, "parquet") == 0 || strcmp(format, "csv") == 0 ||
		strcmp(format, "json") == 0 || strcmp(format, "iceberg") == 0;
}

static bool
is_remote(const char *path)
{
	return strstr(path, "://") != NULL;
}

/* Split a comma-separated location list; blanks around entries are dropped. */
static List *
split_locations(const char *text)
{
	List	   *result = NIL;
	char	   *copy = pstrdup(text);
	char	   *p = copy;

	while (*p)
	{
		char	   *start;
		char	   *end;

		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		start = p;
		while (*p && *p != ',')
			p++;
		end = p;
		while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n'))
			end--;
		if (end > start)
			result = lappend(result, pnstrdup(start, end - start));
		if (*p == ',')
			p++;
	}
	return result;
}

/*
 * The directories gg_duckdb.data_directories allows, each with a trailing
 * slash; NIL when it allows nothing.
 */
static List *
allowed_directories(void)
{
	List	   *dirs = NIL;
	ListCell   *lc;

	if (gg_duckdb_data_directories == NULL || gg_duckdb_data_directories[0] == '\0')
		return NIL;
	foreach(lc, split_locations(gg_duckdb_data_directories))
	{
		char	   *d = (char *) lfirst(lc);

		if (d[strlen(d) - 1] != '/')
			d = psprintf("%s/", d);
		dirs = lappend(dirs, d);
	}
	return dirs;
}

static bool
location_allowed(const char *location, List *dirs)
{
	ListCell   *lc;

	if (location[0] != '/' && !is_remote(location))
		return false;
	foreach(lc, dirs)
	{
		const char *d = (const char *) lfirst(lc);
		size_t		dl = strlen(d);

		/* under the directory, or the directory itself */
		if (strncmp(location, d, dl) == 0 ||
			(strlen(location) == dl - 1 && strncmp(location, d, dl - 1) == 0))
			return true;
	}
	return false;
}

static void check_locations(List *locations);

void
gg_duckdb_check_locations(List *locations)
{
	check_locations(locations);
}

static void
check_locations(List *locations)
{
	List	   *dirs = allowed_directories();
	ListCell   *lc;

	foreach(lc, locations)
	{
		const char *loc = (const char *) lfirst(lc);

		if (strstr(loc, "/../") != NULL || strstr(loc, "'") != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("gg_duckdb: location \"%s\" is not a plain absolute path or URL", loc)));
		if (!location_allowed(loc, dirs))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("gg_duckdb: location \"%s\" is outside gg_duckdb.data_directories", loc),
					 errhint("Set gg_duckdb.data_directories to the directories DuckDB may read on every node.")));
	}
}

static void
apply_options(List *options, GGForeignOptions *o)
{
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "format") == 0)
			o->format = defGetString(def);
		else if (strcmp(def->defname, "location") == 0)
			o->locations = split_locations(defGetString(def));
		else if (strcmp(def->defname, "hive_partitioning") == 0)
			o->hive_partitioning = defGetBoolean(def);
		else if (strcmp(def->defname, "union_by_name") == 0)
			o->union_by_name = defGetBoolean(def);
		else if (strcmp(def->defname, "json_format") == 0)
			o->json_format = defGetString(def);
		else if (is_reader_option(def->defname))
			o->reader_opts = lappend(o->reader_opts, def);
	}
}

/* A catalog table name: namespace.table, neither a path nor a URL. */
static bool
is_catalog_name(const char *loc)
{
	return strchr(loc, '/') == NULL && !is_remote(loc) && strchr(loc, '.') != NULL;
}

static const char *
server_option(ForeignServer *server, const char *name)
{
	ListCell   *lc;

	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, name) == 0)
			return defGetString(def);
	}
	return NULL;
}

/* Table options override server options override wrapper options. */
static void get_options(Oid relid, GGForeignOptions *o);

void
gg_duckdb_foreign_options(Oid relid, GGForeignOptions *o)
{
	get_options(relid, o);
}

static void
get_options(Oid relid, GGForeignOptions *o)
{
	ForeignTable *table = GetForeignTable(relid);
	ForeignServer *server = GetForeignServer(table->serverid);
	ForeignDataWrapper *wrapper = GetForeignDataWrapper(server->fdwid);

	memset(o, 0, sizeof(*o));
	o->format = "parquet";
	o->serverid = server->serverid;
	apply_options(wrapper->options, o);
	apply_options(server->options, o);
	apply_options(table->options, o);
	if (o->locations == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
				 errmsg("gg_duckdb: foreign table \"%s\" has no location option", get_rel_name(relid))));
	if (!format_is_known(o->format))
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
				 errmsg("gg_duckdb: unknown format \"%s\"", o->format)));
	if (strcmp(o->format, "iceberg") == 0 && list_length(o->locations) == 1 &&
		is_catalog_name((const char *) linitial(o->locations)))
	{
		const char *name = (const char *) linitial(o->locations);
		const char *dot = strrchr(name, '.');

		if (server_option(server, "iceberg_endpoint") == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
					 errmsg("gg_duckdb: foreign table \"%s\" names the catalog table \"%s\", but server \"%s\" has no iceberg_endpoint option",
							get_rel_name(relid), name, server->servername)));
		o->catalog = true;
		o->catalog_ref = psprintf("gg_ice_%u.%s.%s", server->serverid,
								  duck_ident(pnstrdup(name, dot - name)), duck_ident(dot + 1));
	}
}

/*
 * The ATTACH of a server's Iceberg REST catalog, credentials from the
 * caller's user mapping: none, a bearer token, or an OAuth2 client.
 */
char *
gg_duckdb_iceberg_attach_sql(Oid serverid)
{
	ForeignServer *server = GetForeignServer(serverid);
	UserMapping *um = NULL;
	const char *endpoint = server_option(server, "iceberg_endpoint");
	const char *warehouse = server_option(server, "iceberg_warehouse");
	const char *auth = server_option(server, "iceberg_auth");
	const char *oauth_uri = server_option(server, "iceberg_oauth2_server_uri");
	const char *token = NULL,
			   *client_id = NULL,
			   *client_secret = NULL;
	StringInfoData sql;
	ListCell   *lc;

	if (endpoint == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OPTION_NAME_NOT_FOUND),
				 errmsg("gg_duckdb: server \"%s\" has no iceberg_endpoint option", server->servername)));
	PG_TRY();
	{
		um = GetUserMapping(GetUserId(), serverid);
	}
	PG_CATCH();
	{
		FlushErrorState();
		um = NULL;
	}
	PG_END_TRY();
	if (um != NULL)
	{
		foreach(lc, um->options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "iceberg_token") == 0)
				token = defGetString(def);
			else if (strcmp(def->defname, "iceberg_client_id") == 0)
				client_id = defGetString(def);
			else if (strcmp(def->defname, "iceberg_client_secret") == 0)
				client_secret = defGetString(def);
		}
	}
	if (auth == NULL)
		auth = token ? "bearer" : client_id ? "oauth2" : "none";

	initStringInfo(&sql);
	appendStringInfo(&sql, "ATTACH IF NOT EXISTS %s AS gg_ice_%u (TYPE iceberg, ENDPOINT %s, AUTHORIZATION_TYPE %s",
					 duck_literal(warehouse ? warehouse : ""), serverid,
					 duck_literal(endpoint), duck_literal(auth));
	if (token)
		appendStringInfo(&sql, ", TOKEN %s", duck_literal(token));
	if (client_id)
		appendStringInfo(&sql, ", CLIENT_ID %s", duck_literal(client_id));
	if (client_secret)
		appendStringInfo(&sql, ", CLIENT_SECRET %s", duck_literal(client_secret));
	if (oauth_uri)
		appendStringInfo(&sql, ", OAUTH2_SERVER_URI %s", duck_literal(oauth_uri));
	appendStringInfoChar(&sql, ')');
	return sql.data;
}

/*
 * Attached catalogs are the instance's: one ATTACH per server and backend,
 * done again (after a DETACH) when the server's or the mapping's options
 * changed, and forgotten with the instance.
 */
static List *attached = NIL;	/* of (serverid Oid as int, ATTACH text), TopMemoryContext */

void
gg_duckdb_fdw_instance_closed(void)
{
	attached = NIL;
}

void
gg_duckdb_iceberg_attach(duckdb_connection conn, Oid serverid)
{
	char	   *sql = gg_duckdb_iceberg_attach_sql(serverid);
	ListCell   *lc;
	List	   *entry = NIL;
	MemoryContext oldcxt;
	duckdb_result res;

	foreach(lc, attached)
	{
		List	   *e = (List *) lfirst(lc);

		if ((Oid) intVal(linitial(e)) == serverid)
		{
			entry = e;
			break;
		}
	}
	if (entry != NULL && strcmp((const char *) lsecond(entry), sql) == 0)
		return;
	if (entry != NULL)
	{
		char	   *detach = psprintf("DETACH gg_ice_%u", serverid);

		if (duckdb_query(conn, detach, &res) == DuckDBError)
		{
			/* not attached after all: nothing to detach */
		}
		duckdb_destroy_result(&res);
		attached = list_delete_ptr(attached, entry);
	}
	if (duckdb_query(conn, sql, &res) == DuckDBError)
	{
		const char *msg = duckdb_result_error(&res);
		char	   *copy = pstrdup(msg ? msg : "unknown error");

		duckdb_destroy_result(&res);
		gg_duckdb_note_error(copy);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("gg_duckdb: could not attach the Iceberg catalog of server \"%s\": %s",
						GetForeignServer(serverid)->servername, copy)));
	}
	duckdb_destroy_result(&res);
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	attached = lappend(attached, list_make2(makeInteger((int) serverid), pstrdup(sql)));
	MemoryContextSwitchTo(oldcxt);
}

Datum
gg_duckdb_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		const GGOptionDef *opt;
		bool		known = false;

		for (opt = valid_options; opt->name; opt++)
			if (catalog == opt->context && strcmp(opt->name, def->defname) == 0)
				known = true;
		if (!known)
		{
			StringInfoData buf;

			initStringInfo(&buf);
			for (opt = valid_options; opt->name; opt++)
				if (catalog == opt->context)
					appendStringInfo(&buf, "%s%s", buf.len > 0 ? ", " : "", opt->name);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 buf.len > 0 ? errhint("Valid options in this context are: %s", buf.data)
					 : errhint("There are no valid options in this context.")));
		}
		if (strcmp(def->defname, "format") == 0 && !format_is_known(defGetString(def)))
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("gg_duckdb: format must be parquet, csv or json")));
		if (strcmp(def->defname, "location") == 0)
		{
			List	   *locs = split_locations(defGetString(def));
			bool		iceberg = false;
			ListCell   *lc2;

			foreach(lc2, options)
			{
				DefElem    *d2 = (DefElem *) lfirst(lc2);

				if (strcmp(d2->defname, "format") == 0 && strcmp(defGetString(d2), "iceberg") == 0)
					iceberg = true;
			}
			if (locs == NIL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("gg_duckdb: location must name at least one file or pattern")));
			/* a catalog table (format iceberg, namespace.table) is the catalog's to check */
			if (!(iceberg && list_length(locs) == 1 && is_catalog_name((const char *) linitial(locs))))
				check_locations(locs);
		}
		if (strcmp(def->defname, "hive_partitioning") == 0 || strcmp(def->defname, "union_by_name") == 0 ||
			strcmp(def->defname, "s3_use_ssl") == 0 || strcmp(def->defname, "insert_dist_by_key") == 0)
			(void) defGetBoolean(def);
		if (strcmp(def->defname, "insert_dist_by_key_weight") == 0)
			(void) defGetInt32(def);
		if (strcmp(def->defname, "json_format") == 0)
		{
			const char *v = defGetString(def);

			if (strcmp(v, "auto") != 0 && strcmp(v, "newline_delimited") != 0 &&
				strcmp(v, "array") != 0 && strcmp(v, "unstructured") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
						 errmsg("gg_duckdb: json_format must be auto, newline_delimited, array or unstructured")));
		}
		if (strcmp(def->defname, "iceberg_auth") == 0)
		{
			const char *v = defGetString(def);

			if (strcmp(v, "none") != 0 && strcmp(v, "bearer") != 0 && strcmp(v, "oauth2") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
						 errmsg("gg_duckdb: iceberg_auth must be none, bearer or oauth2")));
		}
		if (strcmp(def->defname, "s3_url_style") == 0 &&
			strcmp(defGetString(def), "path") != 0 && strcmp(defGetString(def), "vhost") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("gg_duckdb: s3_url_style must be path or vhost")));
	}
	PG_RETURN_VOID();
}

/*
 * The S3 credentials of a server: the endpoint and style from the server,
 * the keys from the caller's user mapping, as a DuckDB secret scoped to the
 * buckets of the table's locations.  Temporary secrets live in the backend's
 * DuckDB instance; creating them again is cheap and picks up changes.
 */
static void ensure_s3_secrets_for_server(Oid serverid, GGForeignOptions *o);

static void
ensure_s3_secrets(Oid relid, GGForeignOptions *o)
{
	ensure_s3_secrets_for_server(GetForeignTable(relid)->serverid, o);
}

void
gg_duckdb_ensure_s3_secrets(Oid relid, GGForeignOptions *o)
{
	ensure_s3_secrets(relid, o);
}

static void
ensure_s3_secrets_for_server(Oid serverid, GGForeignOptions *o)
{
	ForeignServer *server = GetForeignServer(serverid);
	UserMapping *um = NULL;
	const char *endpoint = NULL,
			   *region = NULL,
			   *url_style = NULL,
			   *key_id = NULL,
			   *secret = NULL,
			   *token = NULL;
	bool		use_ssl = true;
	bool		have_ssl = false;
	ListCell   *lc;
	List	   *scopes = NIL;
	bool		any_s3 = false;

	if (o->catalog)
	{
		/* the data files are wherever the catalog says: a secret without a scope */
		any_s3 = true;
		scopes = list_make1(pstrdup(""));
	}
	foreach(lc, o->locations)
	{
		const char *loc = (const char *) lfirst(lc);

		if (strncmp(loc, "s3://", 5) == 0 || strncmp(loc, "s3a://", 6) == 0)
		{
			const char *b = strstr(loc, "://") + 3;
			const char *e = strchr(b, '/');
			char	   *scope = e ? psprintf("s3://%.*s", (int) (e - b), b) : psprintf("s3://%s", b);
			bool		dup = false;
			ListCell   *sc;

			any_s3 = true;
			foreach(sc, scopes)
				if (strcmp((char *) lfirst(sc), scope) == 0)
					dup = true;
			if (!dup)
				scopes = lappend(scopes, scope);
		}
	}
	if (!any_s3)
		return;

	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "s3_endpoint") == 0)
			endpoint = defGetString(def);
		else if (strcmp(def->defname, "s3_region") == 0)
			region = defGetString(def);
		else if (strcmp(def->defname, "s3_url_style") == 0)
			url_style = defGetString(def);
		else if (strcmp(def->defname, "s3_use_ssl") == 0)
		{
			use_ssl = defGetBoolean(def);
			have_ssl = true;
		}
	}
	PG_TRY();
	{
		um = GetUserMapping(GetUserId(), server->serverid);
	}
	PG_CATCH();
	{
		/* no mapping: anonymous access, as DuckDB would do without a secret */
		FlushErrorState();
		um = NULL;
	}
	PG_END_TRY();
	if (um != NULL)
	{
		foreach(lc, um->options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "s3_access_key_id") == 0)
				key_id = defGetString(def);
			else if (strcmp(def->defname, "s3_secret_access_key") == 0)
				secret = defGetString(def);
			else if (strcmp(def->defname, "s3_session_token") == 0)
				token = defGetString(def);
		}
	}
	if (endpoint == NULL && region == NULL && key_id == NULL)
		return;

	foreach(lc, scopes)
	{
		const char *scope = (const char *) lfirst(lc);
		StringInfoData sql;
		List	   *res;

		initStringInfo(&sql);
		if (scope[0] == '\0')
			appendStringInfo(&sql, "CREATE OR REPLACE TEMPORARY SECRET gg_%u_catalog (TYPE s3", server->serverid);
		else
			appendStringInfo(&sql, "CREATE OR REPLACE TEMPORARY SECRET gg_%u_%s (TYPE s3, SCOPE %s",
							 server->serverid, scope + 5, duck_literal(scope));
		if (key_id)
			appendStringInfo(&sql, ", KEY_ID %s", duck_literal(key_id));
		if (secret)
			appendStringInfo(&sql, ", SECRET %s", duck_literal(secret));
		if (token)
			appendStringInfo(&sql, ", SESSION_TOKEN %s", duck_literal(token));
		if (endpoint)
			appendStringInfo(&sql, ", ENDPOINT %s", duck_literal(endpoint));
		if (region)
			appendStringInfo(&sql, ", REGION %s", duck_literal(region));
		if (url_style)
			appendStringInfo(&sql, ", URL_STYLE %s", duck_literal(url_style));
		if (have_ssl)
			appendStringInfo(&sql, ", USE_SSL %s", use_ssl ? "true" : "false");
		appendStringInfoChar(&sql, ')');
		res = duck_text_column(sql.data, "could not create the S3 secret");
		(void) res;
	}
}

/* ---------- DuckDB helpers ---------- */

/* A DuckDB string literal. */
static char *
duck_literal(const char *s)
{
	StringInfoData buf;
	const char *p;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '\'');
	for (p = s; *p; p++)
	{
		if (*p == '\'')
			appendStringInfoChar(&buf, '\'');
		appendStringInfoChar(&buf, *p);
	}
	appendStringInfoChar(&buf, '\'');
	return buf.data;
}

char *
gg_duckdb_duck_literal(const char *s)
{
	return duck_literal(s);
}

/* A DuckDB quoted identifier. */
static char *
duck_ident(const char *s)
{
	StringInfoData buf;
	const char *p;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '"');
	for (p = s; *p; p++)
	{
		if (*p == '"')
			appendStringInfoChar(&buf, '"');
		appendStringInfoChar(&buf, *p);
	}
	appendStringInfoChar(&buf, '"');
	return buf.data;
}

char *
gg_duckdb_duck_ident(const char *s)
{
	return duck_ident(s);
}

/*
 * Run a query on a fresh connection and collect column 0 of every row as
 * text.  Errors are raised as PostgreSQL errors.
 */
static List *
duck_text_column(const char *sql, const char *what)
{
	duckdb_connection conn = gg_duckdb_connect();
	duckdb_result res;
	List	   *result = NIL;
	idx_t		nrows,
				r;

	if (duckdb_query(conn, sql, &res) == DuckDBError)
	{
		const char *msg = duckdb_result_error(&res);
		char	   *copy = pstrdup(msg ? msg : "unknown error");

		duckdb_destroy_result(&res);
		gg_duckdb_disconnect(&conn);
		gg_duckdb_note_error(copy);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("gg_duckdb: %s: %s", what, copy)));
	}
	nrows = duckdb_row_count(&res);
	for (r = 0; r < nrows; r++)
	{
		char	   *val = duckdb_value_varchar(&res, 0, r);

		result = lappend(result, pstrdup(val ? val : ""));
		if (val)
			duckdb_free(val);
	}
	duckdb_destroy_result(&res);
	gg_duckdb_disconnect(&conn);
	return result;
}

/* A two-column text result as a list of two-element lists. */
static List *
duck_text_pairs(const char *sql, const char *what)
{
	duckdb_connection conn = gg_duckdb_connect();
	duckdb_result res;
	List	   *result = NIL;
	idx_t		nrows,
				r;

	if (duckdb_query(conn, sql, &res) == DuckDBError)
	{
		const char *msg = duckdb_result_error(&res);
		char	   *copy = pstrdup(msg ? msg : "unknown error");

		duckdb_destroy_result(&res);
		gg_duckdb_disconnect(&conn);
		gg_duckdb_note_error(copy);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("gg_duckdb: %s: %s", what, copy)));
	}
	nrows = duckdb_row_count(&res);
	for (r = 0; r < nrows; r++)
	{
		char	   *a = duckdb_value_varchar(&res, 0, r);
		char	   *b = duckdb_value_varchar(&res, 1, r);

		result = lappend(result, list_make2(pstrdup(a ? a : ""), pstrdup(b ? b : "")));
		if (a)
			duckdb_free(a);
		if (b)
			duckdb_free(b);
	}
	duckdb_destroy_result(&res);
	gg_duckdb_disconnect(&conn);
	return result;
}

/*
 * The files of a foreign table this process reads: on a segment of an
 * all-segments table, those whose path hash falls to it; everywhere else,
 * all of them.  Sorted, so that the assignment is reproducible.
 */
List *
gg_duckdb_native_files(Oid relid)
{
	GGForeignOptions o;
	ForeignTable *table = GetForeignTable(relid);
	StringInfoData sql;
	ListCell   *lc;
	bool		first = true;
	int			nseg = -1;

	get_options(relid, &o);
	if (o.catalog)
	{
		/* the catalog's files are remote: DuckDB's external access must be on */
		bool		remote = false;

		foreach(lc, allowed_directories())
			if (is_remote((const char *) lfirst(lc)))
				remote = true;
		if (!remote)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("gg_duckdb: catalog table \"%s\" needs a remote prefix in gg_duckdb.data_directories",
							get_rel_name(relid))));
		ensure_s3_secrets(relid, &o);
		{
			duckdb_connection conn = gg_duckdb_connect();

			PG_TRY();
			{
				gg_duckdb_iceberg_attach(conn, o.serverid);
			}
			PG_CATCH();
			{
				gg_duckdb_disconnect(&conn);
				PG_RE_THROW();
			}
			PG_END_TRY();
			gg_duckdb_disconnect(&conn);
		}
	}
	else
	{
		check_locations(o.locations);
		ensure_s3_secrets(relid, &o);
	}
	if (table->exec_location == FTEXECLOCATION_ALL_SEGMENTS &&
		Gp_role == GP_ROLE_EXECUTE && GpIdentity.segindex >= 0 && getgpsegmentCount() > 1)
		nseg = getgpsegmentCount();

	initStringInfo(&sql);
	appendStringInfoString(&sql, "SELECT file FROM (");
	foreach(lc, o.locations)
	{
		/* an Iceberg table is one location, read whole by whichever process it falls to */
		if (strcmp(o.format, "iceberg") == 0)
			appendStringInfo(&sql, "%sSELECT %s AS file", first ? "" : " UNION ALL ",
							 duck_literal((const char *) lfirst(lc)));
		else
			appendStringInfo(&sql, "%sSELECT file FROM glob(%s)", first ? "" : " UNION ALL ",
							 duck_literal((const char *) lfirst(lc)));
		first = false;
	}
	appendStringInfoString(&sql, ") AS g");
	if (nseg > 0)
		appendStringInfo(&sql, " WHERE hash(file) %% %d = %d", nseg, GpIdentity.segindex);
	appendStringInfoString(&sql, " ORDER BY file");
	return duck_text_column(sql.data, "could not list the files of the foreign table");
}

static const char *
reader_function(const char *format)
{
	if (strcmp(format, "csv") == 0)
		return "read_csv";
	if (strcmp(format, "json") == 0)
		return "read_json";
	if (strcmp(format, "iceberg") == 0)
		return "iceberg_scan";
	return "read_parquet";
}

/* "read_parquet(%s, opt=..., ...)" with %s for the file list parameter. */
static char *
reader_call(GGForeignOptions *o)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	if (o->catalog)
	{
		/* the attached catalog's table: no file list to bind */
		appendStringInfoString(&buf, o->catalog_ref);
		return buf.data;
	}
	if (strcmp(o->format, "iceberg") == 0)
	{
		/* the list parameter holds the one table location */
		appendStringInfoString(&buf, "iceberg_scan(%s[1], allow_moved_paths=true)");
		return buf.data;
	}
	appendStringInfo(&buf, "%s(%%s", reader_function(o->format));
	if (strcmp(o->format, "parquet") == 0)
		appendStringInfo(&buf, ", union_by_name=%s, hive_partitioning=%s",
						 o->union_by_name ? "true" : "false",
						 o->hive_partitioning ? "true" : "false");
	else
	{
		appendStringInfo(&buf, ", union_by_name=%s", o->union_by_name ? "true" : "false");
		if (o->hive_partitioning)
			appendStringInfoString(&buf, ", hive_partitioning=true");
		if (strcmp(o->format, "json") == 0)
		{
			/*
			 * DuckDB detects a JSON file's layout with a 32 MB buffer it
			 * keeps per file, more than a table of several files gets
			 * under the memory budget: a pattern location reads as
			 * newline-delimited (what the writer produces) unless told
			 * otherwise, a single file is detected.
			 */
			const char *jf = o->json_format;

			if (jf == NULL)
			{
				ListCell   *lc2;

				foreach(lc2, o->locations)
					if (strpbrk((const char *) lfirst(lc2), "*?[") != NULL)
						jf = "newline_delimited";
			}
			if (jf != NULL && strcmp(jf, "auto") != 0)
				appendStringInfo(&buf, ", format=%s", duck_literal(jf));
		}
	}
	foreach(lc, o->reader_opts)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		const char *v = defGetString(def);

		if (strcmp(def->defname, "header") == 0)
			appendStringInfo(&buf, ", header=%s", parse_bool(v, &(bool){false}) && strcmp(v, "true") == 0 ? "true" :
							 (strcmp(v, "on") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0) ? "true" : "false");
		else if (strcmp(def->defname, "skip") == 0 || strcmp(def->defname, "sample_size") == 0 ||
				 strcmp(def->defname, "maximum_object_size") == 0)
			appendStringInfo(&buf, ", %s=%ld", def->defname, strtol(v, NULL, 10));
		else
			appendStringInfo(&buf, ", %s=%s", def->defname, duck_literal(v));
	}
	appendStringInfoChar(&buf, ')');
	return buf.data;
}

/*
 * How DuckDB reads the table: its column names in the files, the DuckDB
 * shape of every fetched attribute, the reader call and the empty relation.
 */
bool
gg_duckdb_native_describe(Oid relid, List *attnos, GGNativeInfo *info, const char **reject)
{
	Relation	rel = table_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	GGForeignOptions o;
	StringInfoData empty;
	ListCell   *lc;
	int			k = 0;

	memset(info, 0, sizeof(*info));
	info->natts = desc->natts;
	info->colnames = palloc0(sizeof(char *) * Max(desc->natts, 1));
	info->types = palloc0(sizeof(GGTypeInfo) * Max(desc->natts, 1));
	get_options(relid, &o);

	initStringInfo(&empty);
	appendStringInfoString(&empty, "(SELECT ");
	foreach(lc, attnos)
	{
		int			attno = lfirst_int(lc);
		Form_pg_attribute att;
		List	   *colopts;
		ListCell   *oc;
		const char *colname;

		if (attno < 1 || attno > desc->natts)
		{
			*reject = "attribute out of range";
			table_close(rel, AccessShareLock);
			return false;
		}
		att = TupleDescAttr(desc, attno - 1);
		if (att->attisdropped)
		{
			*reject = "dropped column";
			table_close(rel, AccessShareLock);
			return false;
		}
		if (!gg_duckdb_type_map(att->atttypid, att->atttypmod, &info->types[attno - 1]))
		{
			*reject = psprintf("column \"%s\" has type %s, which DuckDB regions do not carry",
							   NameStr(att->attname), format_type_with_typemod(att->atttypid, att->atttypmod));
			table_close(rel, AccessShareLock);
			return false;
		}
		colname = NameStr(att->attname);
		colopts = GetForeignColumnOptions(relid, attno);
		foreach(oc, colopts)
		{
			DefElem    *def = (DefElem *) lfirst(oc);

			if (strcmp(def->defname, "column_name") == 0)
				colname = defGetString(def);
		}
		info->colnames[attno - 1] = duck_ident(colname);
		appendStringInfo(&empty, "%sCAST(NULL AS %s) AS %s", k > 0 ? ", " : "",
						 gg_duckdb_type_sql(&info->types[attno - 1]), info->colnames[attno - 1]);
		k++;
	}
	if (k == 0)
		appendStringInfoString(&empty, "1 AS c0");
	appendStringInfoString(&empty, " WHERE false)");
	info->empty = empty.data;
	info->reader = reader_call(&o);
	table_close(rel, AccessShareLock);
	return true;
}

/*
 * Statements a query with native readers runs first: an Iceberg table read
 * by its directory needs DuckDB's permission to pick the newest metadata
 * file when the table has no version hint (catalog-written tables have
 * none); a concurrent writer's uncommitted metadata could be picked, which
 * is why DuckDB calls it unsafe.  Give the metadata file itself as the
 * location to avoid the guess.
 */
List *
gg_duckdb_native_pre_sql(List *natives)
{
	ListCell   *lc;
	List	   *result = NIL;
	bool		guessing = false;

	foreach(lc, natives)
	{
		GGNativeLeaf *nl = (GGNativeLeaf *) lfirst(lc);

		if (strstr(nl->reader, "iceberg_scan(") != NULL && !guessing)
		{
			result = lappend(result, "SET unsafe_enable_version_guessing = true");
			guessing = true;
		}
		else if (strncmp(nl->reader, "gg_ice_", 7) == 0)
			result = lappend(result, gg_duckdb_iceberg_attach_sql(GetForeignTable(nl->relid)->serverid));
	}
	return result;
}

char *
gg_duckdb_native_location_text(Oid relid)
{
	GGForeignOptions o;
	StringInfoData buf;
	ListCell   *lc;

	get_options(relid, &o);
	initStringInfo(&buf);
	appendStringInfo(&buf, "%s", o.format);
	foreach(lc, o.locations)
		appendStringInfo(&buf, " %s", (const char *) lfirst(lc));
	return buf.data;
}

/* ---------- planning ---------- */

static bool
is_gg_duckdb_table(Oid relid)
{
	ForeignTable *table;
	ForeignServer *server;
	ForeignDataWrapper *wrapper;

	if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_FOREIGN_TABLE)
		return false;
	table = GetForeignTable(relid);
	server = GetForeignServer(table->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);
	return strcmp(wrapper->fdwname, "gg_duckdb") == 0;
}

/* Is this ForeignScan one of ours, with a private list we wrote? */
bool
gg_duckdb_is_native_scan(ForeignScan *fs, List *rtable)
{
	RangeTblEntry *rte;

	if (fs->scan.scanrelid == 0 || fs->scan.scanrelid > list_length(rtable))
		return false;
	rte = rt_fetch(fs->scan.scanrelid, rtable);
	if (rte->rtekind != RTE_RELATION || !is_gg_duckdb_table(rte->relid))
		return false;
	return list_length(fs->fdw_private) == 3 &&
		IsA(linitial(fs->fdw_private), Const) &&
		DatumGetInt32(((Const *) linitial(fs->fdw_private))->constvalue) == GG_FDW_PRIVATE_VERSION;
}

bool
gg_duckdb_foreign_scan_private(ForeignScan *fs, List **attnos, List **quals)
{
	ListCell   *lc;

	if (list_length(fs->fdw_private) != 3)
		return false;
	*attnos = NIL;
	foreach(lc, (List *) lsecond(fs->fdw_private))
		*attnos = lappend_int(*attnos, DatumGetInt32(((Const *) lfirst(lc))->constvalue));
	*quals = (List *) lthird(fs->fdw_private);
	return true;
}

static Const *
make_int_const(int v)
{
	return makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(v), false, true);
}

static void
fdw_get_rel_size(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	if (baserel->tuples <= 0 && Gp_role == GP_ROLE_DISPATCH)
	{
		/* no statistics yet: the Parquet metadata knows, when the coordinator sees the files */
		GGForeignOptions o;

		get_options(foreigntableid, &o);
		if (strcmp(o.format, "parquet") == 0)
		{
			MemoryContext oldcxt = CurrentMemoryContext;

			PG_TRY();
			{
				List	   *files = gg_duckdb_native_files(foreigntableid);

				if (files != NIL)
				{
					StringInfoData sql;
					ListCell   *lc;
					bool		first = true;
					List	   *rows;

					initStringInfo(&sql);
					appendStringInfoString(&sql, "SELECT CAST(sum(num_rows) AS VARCHAR) FROM parquet_file_metadata([");
					foreach(lc, files)
					{
						appendStringInfo(&sql, "%s%s", first ? "" : ", ", duck_literal((const char *) lfirst(lc)));
						first = false;
					}
					appendStringInfoString(&sql, "])");
					rows = duck_text_column(sql.data, "could not read the Parquet metadata");
					if (rows != NIL)
						baserel->tuples = strtod((const char *) linitial(rows), NULL);
				}
			}
			PG_CATCH();
			{
				MemoryContextSwitchTo(oldcxt);
				FlushErrorState();
			}
			PG_END_TRY();
		}
		if (baserel->tuples <= 0)
			baserel->tuples = 10000;
	}
	set_baserel_size_estimates(root, baserel);
}

static void
fdw_get_paths(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid)
{
	ForeignPath *path;
	Cost		startup = 100.0;
	Cost		total = startup + baserel->rows * cpu_tuple_cost * 2.0 +
	baserel->tuples * cpu_operator_cost;

	path = create_foreignscan_path(root, baserel, NULL, baserel->rows, startup, total,
								   NIL, NULL, NULL, NIL);
	path->path.locus = cdbpathlocus_from_baserel(root, baserel);
	path->path.motionHazard = false;
	path->path.rescannable = true;
	path->path.sameslice_relids = baserel->relids;
	add_path(baserel, (Path *) path);
	set_cheapest(baserel);
}

static ForeignScan *
fdw_get_plan(PlannerInfo *root, RelOptInfo *baserel, Oid foreigntableid,
			 ForeignPath *best_path, List *tlist, List *scan_clauses, Plan *outer_plan)
{
	Index		scan_relid = baserel->relid;
	Bitmapset  *attrs_used = NULL;
	List	   *attnos = NIL;
	List	   *attno_consts = NIL;
	List	   *pushed = NIL;
	List	   *local = NIL;
	List	   *fdw_private;
	ListCell   *lc;
	int			attno;

	/* the columns the query needs: its target list and every clause */
	pull_varattnos((Node *) baserel->reltarget->exprs, scan_relid, &attrs_used);
	foreach(lc, scan_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		pull_varattnos((Node *) rinfo->clause, scan_relid, &attrs_used);
	}
	attno = -1;
	while ((attno = bms_next_member(attrs_used, attno)) >= 0)
	{
		int			a = attno + FirstLowInvalidHeapAttributeNumber;

		if (a > 0)
		{
			attnos = lappend_int(attnos, a);
			attno_consts = lappend(attno_consts, make_int_const(a));
		}
		else if (a == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gg_duckdb: whole-row references to a foreign table are not supported")));
	}

	foreach(lc, scan_clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		ListCell   *lc2;

		if (rinfo->pseudoconstant)
			continue;

		/*
		 * ORCA hands the scan its quals as one conjunction; each conjunct
		 * is pushed or kept on its own, as the planner's separate clauses
		 * are.
		 */
		foreach(lc2, make_ands_implicit((Expr *) rinfo->clause))
		{
			Node	   *clause = (Node *) lfirst(lc2);

			if (gg_duckdb_native_qual_ok(foreigntableid, scan_relid, attnos, clause))
				pushed = lappend(pushed, clause);
			else
				local = lappend(local, clause);
		}
	}

	fdw_private = list_make3(make_int_const(GG_FDW_PRIVATE_VERSION), attno_consts, pushed);
	return make_foreignscan(tlist, local, scan_relid, NIL, fdw_private, NIL, NIL, outer_plan);
}

/* ---------- execution ---------- */

typedef struct GGForeignState
{
	GGDuckQuery q;
	Oid			relid;
	List	   *attnos;
	char	   *sql;			/* with the reader token */
	char	   *reader;
	char	   *empty;
	bool		no_files;		/* this node has nothing to read */
} GGForeignState;

static void
fdw_explain_end(PlanState *planstate, struct StringInfoData *buf)
{
	GGForeignState *st = (GGForeignState *) ((ForeignScanState *) planstate)->fdw_state;

	if (st == NULL || (!st->q.started && st->q.rows_out == 0))
		return;
	gg_duckdb_query_explain_end(&st->q, buf);
	appendStringInfo(buf, "; files: %d\n", st->no_files ? 0 : list_length(st->q.file_lists) > 0 ?
					 list_length((List *) linitial(st->q.file_lists)) : 0);
}

static void
fdw_begin(ForeignScanState *node, int eflags)
{
	ForeignScan *fs = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	RangeTblEntry *rte = rt_fetch(fs->scan.scanrelid, estate->es_range_table);
	GGForeignState *st;
	GGNativeScan ns;
	List	   *params = NIL;
	GGTypeInfo *outtypes;
	int		   *colmap;
	ListCell   *lc;
	int			k;

	st = palloc0(sizeof(GGForeignState));
	node->fdw_state = st;
	st->relid = rte->relid;
	gg_duckdb_query_init(&st->q, &node->ss.ps, estate->es_query_cxt, "foreign scan");

	memset(&ns, 0, sizeof(ns));
	ns.relid = rte->relid;
	ns.scanrelid = fs->scan.scanrelid;
	if (!gg_duckdb_foreign_scan_private(fs, &ns.attnos, &ns.quals))
		elog(ERROR, "gg_duckdb: malformed foreign scan");
	if (!gg_duckdb_native_scan_sql(&ns, psprintf(GG_NATIVE_TOKEN_FMT, 0), &params))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: cannot read foreign table \"%s\": %s",
						get_rel_name(rte->relid), ns.reject ? ns.reject : "not deparsable")));
	st->attnos = ns.attnos;
	st->sql = ns.sql;
	st->reader = ns.reader;
	st->empty = ns.empty;
	st->q.params = params;
	if (strstr(ns.reader, "iceberg_scan(") != NULL)
		st->q.pre_sql = list_make1("SET unsafe_enable_version_guessing = true");
	else if (strncmp(ns.reader, "gg_ice_", 7) == 0)
		st->q.pre_sql = list_make1(gg_duckdb_iceberg_attach_sql(GetForeignTable(rte->relid)->serverid));

	/* result column k is attribute attnos[k] of the scan tuple */
	outtypes = palloc0(sizeof(GGTypeInfo) * Max(list_length(ns.attnos), 1));
	colmap = palloc0(sizeof(int) * Max(list_length(ns.attnos), 1));
	k = 0;
	foreach(lc, ns.attnos)
	{
		int			attno = lfirst_int(lc);

		outtypes[k] = ns.types[attno - 1];
		colmap[k] = attno - 1;
		k++;
	}
	if (k == 0)
	{
		/* nothing to fetch (count(*)): the reader's placeholder column, discarded */
		gg_duckdb_type_map(INT4OID, -1, &outtypes[0]);
		colmap[0] = -1;
		k = 1;
	}
	gg_duckdb_query_set_output(&st->q, node->ss.ss_ScanTupleSlot->tts_tupleDescriptor,
							   outtypes, k, colmap);

	if (estate->es_instrument)
	{
		node->ss.ps.cdbexplainbuf = makeStringInfo();
		node->ss.ps.cdbexplainfun = fdw_explain_end;
	}
}

static void
fdw_start(GGForeignState *st)
{
	List	   *files = gg_duckdb_native_files(st->relid);
	char	   *token = psprintf(GG_NATIVE_TOKEN_FMT, 0);
	char	   *at = strstr(st->sql, token);
	StringInfoData sql;
	char	   *reader;

	if (at == NULL)
		elog(ERROR, "gg_duckdb: the foreign scan query has no reader");
	if (files == NIL)
	{
		st->no_files = true;
		st->q.finished = true;
		st->q.started = true;
		return;
	}
	if (strstr(st->reader, "%s") != NULL)
	{
		reader = psprintf(st->reader, psprintf("$%d", list_length(st->q.params) + 1));
		st->q.file_lists = list_make1(files);
	}
	else
	{
		reader = st->reader;	/* a catalog table: nothing to bind */
		st->q.file_lists = NIL;
	}
	initStringInfo(&sql);
	appendBinaryStringInfo(&sql, st->sql, at - st->sql);
	appendStringInfoString(&sql, reader);
	appendStringInfoString(&sql, at + strlen(token));
	st->q.sql = sql.data;
	gg_duckdb_query_start(&st->q, NULL);
}

static TupleTableSlot *
fdw_iterate(ForeignScanState *node)
{
	GGForeignState *st = (GGForeignState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	if (!st->q.started)
		fdw_start(st);
	if (st->q.finished)
		return ExecClearTuple(slot);
	gg_duckdb_query_next(&st->q, slot);
	return slot;
}

static void
fdw_rescan(ForeignScanState *node)
{
	GGForeignState *st = (GGForeignState *) node->fdw_state;

	gg_duckdb_query_reset(&st->q);
	st->no_files = false;
}

static void
fdw_end(ForeignScanState *node)
{
	GGForeignState *st = (GGForeignState *) node->fdw_state;

	if (st == NULL)
		return;
	gg_duckdb_query_release(&st->q);
	if (gg_duckdb_release_instance_at_end)
		gg_duckdb_instance_close();
}

static void
fdw_explain(ForeignScanState *node, ExplainState *es)
{
	GGForeignState *st = (GGForeignState *) node->fdw_state;

	if (st == NULL)
		return;
	ExplainPropertyText("DuckDB reader", gg_duckdb_native_location_text(st->relid), es);
	if (es->verbose)
		ExplainPropertyText("DuckDB SQL", st->sql, es);
}

/* ---------- ANALYZE ---------- */

/*
 * A reservoir sample of the rows this node reads, through DuckDB's own
 * sampling; the row count from a count over the same files.
 */
static int
fdw_acquire_sample_rows(Relation onerel, int elevel, HeapTuple *rows, int targrows,
						double *totalrows, double *totaldeadrows)
{
	Oid			relid = RelationGetRelid(onerel);
	TupleDesc	desc = RelationGetDescr(onerel);
	List	   *files = gg_duckdb_native_files(relid);
	GGNativeScan ns;
	List	   *params = NIL;
	List	   *attnos = NIL;
	GGDuckQuery *q;
	GGTypeInfo *outtypes;
	int		   *colmap;
	TupleTableSlot *slot;
	char	   *token = psprintf(GG_NATIVE_TOKEN_FMT, 0);
	char	   *at;
	StringInfoData sql;
	int			numrows = 0;
	int			i,
				k;
	ListCell   *lc;

	*totalrows = 0;
	*totaldeadrows = 0;
	if (files == NIL)
		return 0;

	for (i = 0; i < desc->natts; i++)
		if (!TupleDescAttr(desc, i)->attisdropped)
			attnos = lappend_int(attnos, i + 1);
	memset(&ns, 0, sizeof(ns));
	ns.relid = relid;
	ns.attnos = attnos;
	if (!gg_duckdb_native_scan_sql(&ns, token, &params))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: cannot read foreign table \"%s\": %s",
						RelationGetRelationName(onerel), ns.reject ? ns.reject : "not deparsable")));

	/* the row count first */
	{
		StringInfoData csql;

		initStringInfo(&csql);
		at = strstr(ns.sql, token);
		appendStringInfoString(&csql, "SELECT CAST(count(*) AS VARCHAR) FROM (");
		appendBinaryStringInfo(&csql, ns.sql, at - ns.sql);
		appendStringInfoString(&csql, psprintf(ns.reader, "$1"));
		appendStringInfoString(&csql, at + strlen(token));
		appendStringInfoString(&csql, ") AS t");
		/* the query state must outlive its memory context's reset callback */
		q = palloc0(sizeof(GGDuckQuery));
		gg_duckdb_query_init(q, NULL, CurrentMemoryContext, "analyze");
		q->sql = csql.data;
		q->file_lists = list_make1(files);
		if (strstr(ns.reader, "iceberg_scan(") != NULL)
			q->pre_sql = list_make1("SET unsafe_enable_version_guessing = true");
		outtypes = palloc0(sizeof(GGTypeInfo));
		gg_duckdb_type_map(TEXTOID, -1, &outtypes[0]);
		{
			TupleDesc	cdesc = CreateTemplateTupleDesc(1);

			TupleDescInitEntry(cdesc, 1, "n", TEXTOID, -1, 0);
			slot = MakeSingleTupleTableSlot(cdesc, &TTSOpsVirtual);
			gg_duckdb_query_set_output(q, cdesc, outtypes, 1, NULL);
			gg_duckdb_query_start(q, NULL);
			if (gg_duckdb_query_next(q, slot) && !slot->tts_isnull[0])
				*totalrows = strtod(TextDatumGetCString(slot->tts_values[0]), NULL);
			gg_duckdb_query_release(q);
			ExecDropSingleTupleTableSlot(slot);
		}
	}

	/* then the sample */
	initStringInfo(&sql);
	at = strstr(ns.sql, token);
	appendBinaryStringInfo(&sql, ns.sql, at - ns.sql);
	appendStringInfoString(&sql, psprintf(ns.reader, "$1"));
	appendStringInfoString(&sql, at + strlen(token));
	appendStringInfo(&sql, " USING SAMPLE reservoir(%d ROWS) REPEATABLE (42)", targrows);
	q = palloc0(sizeof(GGDuckQuery));
	gg_duckdb_query_init(q, NULL, CurrentMemoryContext, "analyze");
	q->sql = sql.data;
	q->file_lists = list_make1(files);
	if (strstr(ns.reader, "iceberg_scan(") != NULL)
		q->pre_sql = list_make1("SET unsafe_enable_version_guessing = true");
	outtypes = palloc0(sizeof(GGTypeInfo) * Max(list_length(attnos), 1));
	colmap = palloc0(sizeof(int) * Max(list_length(attnos), 1));
	k = 0;
	foreach(lc, attnos)
	{
		int			attno = lfirst_int(lc);

		outtypes[k] = ns.types[attno - 1];
		colmap[k] = attno - 1;
		k++;
	}
	slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	gg_duckdb_query_set_output(q, desc, outtypes, list_length(attnos), colmap);
	gg_duckdb_query_start(q, NULL);
	while (numrows < targrows && gg_duckdb_query_next(q, slot))
	{
		vacuum_delay_point();
		rows[numrows++] = heap_form_tuple(desc, slot->tts_values, slot->tts_isnull);
	}
	gg_duckdb_query_release(q);
	ExecDropSingleTupleTableSlot(slot);

	if (*totalrows < numrows)
		*totalrows = numrows;
	ereport(elevel,
			(errmsg("\"%s\": %d files, %.0f rows; %d rows in sample",
					RelationGetRelationName(onerel), list_length(files), *totalrows, numrows)));
	return numrows;
}

static bool
fdw_analyze(Relation relation, AcquireSampleRowsFunc *func, BlockNumber *totalpages)
{
	ForeignTable *table = GetForeignTable(RelationGetRelid(relation));

	*totalpages = 10;
	if (Gp_role == GP_ROLE_DISPATCH && table->exec_location == FTEXECLOCATION_ALL_SEGMENTS)
	{
		/* the segments sample their own files; the coordinator merges */
		*func = gp_acquire_sample_rows_func;
		return true;
	}
	*func = fdw_acquire_sample_rows;
	return true;
}

/* ---------- IMPORT FOREIGN SCHEMA ---------- */

/* PostgreSQL's spelling of a DuckDB column type, or NULL when not carried. */
static const char *
pg_type_for_duck(const char *t)
{
	if (strcmp(t, "BOOLEAN") == 0)
		return "boolean";
	if (strcmp(t, "TINYINT") == 0 || strcmp(t, "SMALLINT") == 0 || strcmp(t, "UTINYINT") == 0)
		return "smallint";
	if (strcmp(t, "INTEGER") == 0 || strcmp(t, "USMALLINT") == 0)
		return "integer";
	if (strcmp(t, "BIGINT") == 0 || strcmp(t, "UINTEGER") == 0)
		return "bigint";
	if (strcmp(t, "HUGEINT") == 0 || strcmp(t, "UBIGINT") == 0 || strcmp(t, "UHUGEINT") == 0)
		return "numeric(38,0)";
	if (strcmp(t, "FLOAT") == 0)
		return "real";
	if (strcmp(t, "DOUBLE") == 0)
		return "double precision";
	if (strcmp(t, "VARCHAR") == 0)
		return "text";
	if (strcmp(t, "BLOB") == 0)
		return "bytea";
	if (strcmp(t, "DATE") == 0)
		return "date";
	if (strcmp(t, "TIMESTAMP") == 0 || strcmp(t, "TIMESTAMP_S") == 0 ||
		strcmp(t, "TIMESTAMP_MS") == 0 || strcmp(t, "TIMESTAMP_NS") == 0)
		return "timestamp";
	if (strcmp(t, "TIMESTAMP WITH TIME ZONE") == 0)
		return "timestamp with time zone";
	if (strcmp(t, "INTERVAL") == 0)
		return "interval";
	if (strcmp(t, "UUID") == 0)
		return "uuid";
	if (strncmp(t, "DECIMAL(", 8) == 0)
	{
		int			p,
					s;

		if (sscanf(t, "DECIMAL(%d,%d)", &p, &s) == 2 && p <= 38)
			return psprintf("numeric(%d,%d)", p, s);
	}
	return NULL;
}

static List *
fdw_import_schema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	ForeignServer *server = GetForeignServer(serverOid);
	List	   *commands = NIL;
	char	   *dir = stmt->remote_schema;
	const char *format = "parquet";
	const char *ext;
	ListCell   *lc;
	List	   *entries = NIL;	/* of (name, location) */
	List	   *seen = NIL;
	List	   *files;

	foreach(lc, stmt->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "format") == 0)
			format = defGetString(def);
		else
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname)));
	}
	if (server_option(server, "iceberg_endpoint") != NULL && strchr(dir, '/') == NULL && !is_remote(dir))
	{
		/* a namespace of the server's Iceberg catalog: one foreign table per table */
		duckdb_connection conn = gg_duckdb_connect();
		List	   *tables;
		GGForeignOptions o;

		memset(&o, 0, sizeof(o));
		o.catalog = true;
		ensure_s3_secrets_for_server(serverOid, &o);
		PG_TRY();
		{
			gg_duckdb_iceberg_attach(conn, serverOid);
		}
		PG_CATCH();
		{
			gg_duckdb_disconnect(&conn);
			PG_RE_THROW();
		}
		PG_END_TRY();
		gg_duckdb_disconnect(&conn);
		tables = duck_text_column(psprintf("SELECT table_name FROM duckdb_tables() WHERE database_name = 'gg_ice_%u' AND schema_name = %s ORDER BY 1",
										   serverOid, duck_literal(dir)),
								  "could not list the catalog's tables");
		foreach(lc, tables)
		{
			char	   *name = (char *) lfirst(lc);
			List	   *cols;
			ListCell   *cc;
			StringInfoData cmd;
			bool		first = true;

			cols = duck_text_pairs(psprintf("SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM gg_ice_%u.%s.%s)",
											serverOid, duck_ident(dir), duck_ident(name)),
								   "could not describe the catalog table");
			initStringInfo(&cmd);
			appendStringInfo(&cmd, "CREATE FOREIGN TABLE %s (", quote_identifier(name));
			foreach(cc, cols)
			{
				List	   *col = (List *) lfirst(cc);
				const char *pgtype = pg_type_for_duck((char *) lsecond(col));

				if (pgtype == NULL)
				{
					ereport(NOTICE,
							(errmsg("gg_duckdb: column \"%s\" of \"%s\" has DuckDB type %s, which is not carried; skipped",
									(char *) linitial(col), name, (char *) lsecond(col))));
					continue;
				}
				appendStringInfo(&cmd, "%s\n    %s %s", first ? "" : ",",
								 quote_identifier((char *) linitial(col)), pgtype);
				first = false;
			}
			appendStringInfo(&cmd, "\n) SERVER %s OPTIONS (location %s, format 'iceberg')",
							 quote_identifier(server->servername),
							 quote_literal_cstr(psprintf("%s.%s", dir, name)));
			commands = lappend(commands, cmd.data);
		}
		return commands;
	}
	if (!format_is_known(format) || strcmp(format, "iceberg") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
				 errmsg("gg_duckdb: IMPORT FOREIGN SCHEMA supports parquet, csv and json")));
	ext = format;
	if (dir[strlen(dir) - 1] == '/')
		dir = pnstrdup(dir, strlen(dir) - 1);
	check_locations(list_make1(dir));
	if (is_remote(dir))
	{
		GGForeignOptions o;

		memset(&o, 0, sizeof(o));
		o.locations = list_make1(dir);
		ensure_s3_secrets_for_server(serverOid, &o);
	}

	/* files right in the directory: one table each; subdirectories: one table each */
	files = duck_text_column(psprintf("SELECT file FROM glob(%s) ORDER BY file",
									  duck_literal(psprintf("%s/*.%s", dir, ext))),
							 "could not list the directory");
	foreach(lc, files)
	{
		char	   *file = (char *) lfirst(lc);
		char	   *base = pstrdup(strrchr(file, '/') + 1);

		base[strlen(base) - strlen(ext) - 1] = '\0';
		entries = lappend(entries, list_make2(base, file));
	}
	files = duck_text_column(psprintf("SELECT DISTINCT split_part(file[%d:], '/', 1) FROM glob(%s) ORDER BY 1",
									  (int) strlen(dir) + 2, duck_literal(psprintf("%s/*/*.%s", dir, ext))),
							 "could not list the directory");
	foreach(lc, files)
	{
		char	   *sub = (char *) lfirst(lc);

		entries = lappend(entries, list_make2(sub, psprintf("%s/%s/*.%s", dir, sub, ext)));
	}

	foreach(lc, entries)
	{
		List	   *e = (List *) lfirst(lc);
		char	   *name = (char *) linitial(e);
		char	   *location = (char *) lsecond(e);
		List	   *cols;
		ListCell   *cc;
		StringInfoData cmd;
		bool		first = true;
		bool		dup = false;
		ListCell   *sc;

		foreach(sc, seen)
			if (strcmp((char *) lfirst(sc), name) == 0)
				dup = true;
		if (dup)
			continue;
		seen = lappend(seen, name);

		cols = duck_text_pairs(psprintf("SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM %s(%s))",
										reader_function(format), duck_literal(location)),
							   "could not describe the files");
		initStringInfo(&cmd);
		appendStringInfo(&cmd, "CREATE FOREIGN TABLE %s (", quote_identifier(name));
		foreach(cc, cols)
		{
			List	   *col = (List *) lfirst(cc);
			const char *pgtype = pg_type_for_duck((char *) lsecond(col));

			if (pgtype == NULL)
			{
				ereport(NOTICE,
						(errmsg("gg_duckdb: column \"%s\" of \"%s\" has DuckDB type %s, which is not carried; skipped",
								(char *) linitial(col), name, (char *) lsecond(col))));
				continue;
			}
			appendStringInfo(&cmd, "%s\n    %s %s", first ? "" : ",",
							 quote_identifier((char *) linitial(col)), pgtype);
			first = false;
		}
		appendStringInfo(&cmd, "\n) SERVER %s OPTIONS (location %s, format %s)",
						 quote_identifier(server->servername), quote_literal_cstr(location),
						 quote_literal_cstr(format));
		commands = lappend(commands, cmd.data);
	}
	return commands;
}

/* ---------- the handler ---------- */

Datum
gg_duckdb_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	routine->GetForeignRelSize = fdw_get_rel_size;
	routine->GetForeignPaths = fdw_get_paths;
	routine->GetForeignPlan = fdw_get_plan;
	routine->BeginForeignScan = fdw_begin;
	routine->IterateForeignScan = fdw_iterate;
	routine->ReScanForeignScan = fdw_rescan;
	routine->EndForeignScan = fdw_end;
	routine->ExplainForeignScan = fdw_explain;
	routine->AnalyzeForeignTable = fdw_analyze;
	routine->ImportForeignSchema = fdw_import_schema;

	/* INSERT (write.c) */
	routine->IsForeignRelUpdatable = gg_duckdb_fdw_updatable;
	routine->BeginForeignModify = gg_duckdb_fdw_begin_modify;
	routine->ExecForeignInsert = gg_duckdb_fdw_insert;
	routine->EndForeignModify = gg_duckdb_fdw_end_modify;
	routine->BeginForeignInsert = gg_duckdb_fdw_begin_insert;
	routine->EndForeignInsert = gg_duckdb_fdw_end_insert;
	routine->ExplainForeignModify = gg_duckdb_fdw_explain_modify;
	PG_RETURN_POINTER(routine);
}

/*
 * gg_duckdb.foreign_files(regclass): the files this segment reads of the
 * table, with the segment's id.  Declared EXECUTE ON ALL SEGMENTS, so one
 * call lists the assignment of the whole cluster.
 */
Datum
gg_duckdb_foreign_files(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	List	   *files;

	if (SRF_IS_FIRSTCALL())
	{
		Oid			relid = PG_GETARG_OID(0);
		MemoryContext oldcxt;
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		if (!is_gg_duckdb_table(relid))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("gg_duckdb: \"%s\" is not a gg_duckdb foreign table", get_rel_name(relid))));
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = gg_duckdb_native_files(relid);
		MemoryContextSwitchTo(oldcxt);
	}
	funcctx = SRF_PERCALL_SETUP();
	files = (List *) funcctx->user_fctx;
	if (files != NIL)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};
		HeapTuple	tuple;

		values[0] = Int32GetDatum(GpIdentity.segindex);
		values[1] = PointerGetDatum(cstring_to_text((const char *) linitial(files)));
		funcctx->user_fctx = list_delete_first(files);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	SRF_RETURN_DONE(funcctx);
}
