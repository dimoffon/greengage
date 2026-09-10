/*-------------------------------------------------------------------------
 *
 * write.c
 *	  INSERT into gg_duckdb foreign tables.
 *
 * Every process that receives rows for a table (a segment of an
 * all-segments table, the coordinator otherwise) buffers them in a table
 * of its own DuckDB instance for the length of the transaction, and writes
 * them as one file when the transaction commits, or prepares on a segment
 * of a distributed transaction: DuckDB's COPY does the writing, so the file
 * formats and the stores are the reader's; an Iceberg catalog table gets one
 * INSERT, and so one snapshot, from the coordinator.  A transaction that aborts before
 * that drops the buffer and writes nothing; a savepoint that is rolled back
 * takes its rows out of the buffer.  Rows of an open transaction are not
 * visible to its own reads.
 *
 * The buffer is a DuckDB table under the instance's memory limit, so large
 * inserts spill to DuckDB's temporary directory.  It is filled through the
 * C API appender, chunk by chunk, with the same value writers the leaf
 * table function uses.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "cdb/cdbtm.h"
#include "cdb/cdbvars.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/vmem_tracker.h"

#include "gg_duckdb/gg_duckdb.h"

/* the subtransaction tag column of the buffer */
#define GG_SUB_COLUMN "_gg_sub"

typedef struct GGWriter
{
	Oid			relid;
	char	   *relname;
	char	   *format;
	bool		catalog;		/* target is an Iceberg catalog table, not a file */
	Oid			serverid;
	char	   *target;			/* the file written at commit, or the catalog table */
	List	   *copy_opts;		/* DefElems: header, delim, compression, ... */

	int			ncols;			/* written columns */
	int		   *attnos;			/* 1-based attribute of each */
	GGTypeInfo *types;
	char	  **colnames;		/* quoted identifiers of the file's columns */

	duckdb_connection conn;
	char	   *table;			/* the buffer table */
	duckdb_appender appender;
	duckdb_data_chunk chunk;
	duckdb_logical_type *ltypes;
	GGVectorTarget *targets;	/* into the chunk's vectors, refreshed after every reset */
	idx_t		nbuf;			/* rows in the chunk */
	int64		rows;			/* appended, aborted savepoints included */
	int64		memory_reserved;
	int64		budget_kb;		/* DuckDB's memory limit while this writer runs */
	int64		bytes;			/* of the buffered rows, roughly: fixed widths and string lengths */
	SubTransactionId max_sub;	/* the newest subtransaction that inserted rows */
	bool		poisoned;		/* a savepoint's rows could not be taken out */
	char	   *poison;

	struct GGWriter *next;
} GGWriter;

static GGWriter *writers = NULL;	/* of the current transaction */
static MemoryContext writers_cxt = NULL;
static bool callbacks_registered = false;

static void writer_xact(XactEvent event, void *arg);
static void writer_subxact(SubXactEvent event, SubTransactionId mySubid,
						   SubTransactionId parentSubid, void *arg);

/* ---------- the target file ---------- */

/*
 * Where a table's new file goes.  The table must have one location whose
 * last path component holds one '*' and no other wildcard: the file takes
 * the wildcard's place, so that the reader's pattern finds it.  NULL with
 * the reason when the table cannot be written.
 */
char *
gg_duckdb_write_target(GGForeignOptions *o, const char *unique, const char **reason)
{
	const char *loc;
	const char *base;
	const char *star;

	*reason = NULL;
	if (o->catalog)
		return pstrdup(o->catalog_ref);
	if (strcmp(o->format, "iceberg") == 0)
	{
		*reason = "an Iceberg table is written through its catalog: name it namespace.table on a server with iceberg_endpoint";
		return NULL;
	}
	if (list_length(o->locations) != 1)
	{
		*reason = "the table has more than one location";
		return NULL;
	}
	loc = (const char *) linitial(o->locations);
	base = strrchr(loc, '/');
	base = base ? base + 1 : loc;
	star = strchr(base, '*');
	if (star == NULL || strchr(star + 1, '*') != NULL ||
		strchr(base, '?') != NULL || strchr(base, '[') != NULL ||
		strchr(base, '{') != NULL)
	{
		*reason = "the location's file name needs exactly one '*' (a pattern such as dir/*.parquet)";
		return NULL;
	}
	if (strstr(loc, "**") != NULL)
	{
		*reason = "the location descends into subdirectories";
		return NULL;
	}
	return psprintf("%.*s%s%s", (int) (star - loc), loc, unique, star + 1);
}

static const char *
unique_name(void)
{
	DistributedTransactionId dxid = getDistributedTransactionId();
	char		seg[16];

	if (GpIdentity.segindex < 0)
		strlcpy(seg, "qd", sizeof(seg));
	else
		snprintf(seg, sizeof(seg), "%d", GpIdentity.segindex);
	if (dxid != 0)
		return psprintf("gg-" UINT64_FORMAT "-%s", (uint64) dxid, seg);
	return psprintf("gg-x%u-%s-%ld", GetTopTransactionId(), seg, (long) time(NULL));
}

/* ---------- DuckDB side ---------- */

static void
writer_raise(GGWriter *w, const char *what, const char *msg)
{
	char	   *copy = pstrdup(msg ? msg : "unknown error");

	gg_duckdb_note_error(copy);
	ereport(ERROR,
			(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			 errmsg("gg_duckdb: %s \"%s\": %s", what, w->relname, copy)));
}

/* Run a statement on the writer's connection; errors are PostgreSQL errors. */
static void
writer_exec(GGWriter *w, const char *sql, const char *what)
{
	duckdb_result res;

	if (duckdb_query(w->conn, sql, &res) == DuckDBError)
	{
		const char *msg = duckdb_result_error(&res);
		char	   *copy = pstrdup(msg ? msg : "unknown error");

		duckdb_destroy_result(&res);
		writer_raise(w, what, copy);
	}
	duckdb_destroy_result(&res);
}

/* The same without raising: the error text, or NULL. */
static char *
writer_try(GGWriter *w, const char *sql)
{
	duckdb_result res;
	char	   *err = NULL;

	if (duckdb_query(w->conn, sql, &res) == DuckDBError)
	{
		const char *msg = duckdb_result_error(&res);

		err = pstrdup(msg ? msg : "unknown error");
	}
	duckdb_destroy_result(&res);
	return err;
}

/* Release DuckDB objects and memory; idempotent, never raises. */
static void
writer_release(GGWriter *w)
{
	int			i;

	if (w->chunk)
	{
		duckdb_destroy_data_chunk(&w->chunk);
		w->chunk = NULL;
	}
	if (w->appender)
	{
		duckdb_appender_destroy(&w->appender);
		w->appender = NULL;
	}
	if (w->ltypes)
	{
		for (i = 0; i <= w->ncols; i++)
			if (w->ltypes[i])
				duckdb_destroy_logical_type(&w->ltypes[i]);
		w->ltypes = NULL;
	}
	if (w->conn)
	{
		if (w->table)
		{
			char	   *err = writer_try(w, psprintf("DROP TABLE IF EXISTS %s", w->table));

			if (err)
				pfree(err);
		}
		gg_duckdb_disconnect(&w->conn);
	}
	if (w->memory_reserved > 0)
	{
		VmemTracker_ReleaseVmem(w->memory_reserved);
		w->memory_reserved = 0;
	}
}

static void
release_all(void)
{
	GGWriter   *w;

	for (w = writers; w != NULL; w = w->next)
		writer_release(w);
	writers = NULL;
	if (writers_cxt)
		MemoryContextReset(writers_cxt);
}

/* The chunk's vectors, resolved again after every reset. */
static void
refresh_targets(GGWriter *w)
{
	int			i;
	GGTypeInfo	sub;

	for (i = 0; i < w->ncols; i++)
		gg_duckdb_vector_target(duckdb_data_chunk_get_vector(w->chunk, i), &w->types[i], &w->targets[i]);
	gg_duckdb_type_map(INT8OID, -1, &sub);	/* any non-STRUCT type: plain data and validity */
	gg_duckdb_vector_target(duckdb_data_chunk_get_vector(w->chunk, w->ncols), &sub, &w->targets[w->ncols]);
}

/* The buffered chunk into the appender. */
static void
writer_flush_chunk(GGWriter *w)
{
	if (w->nbuf == 0)
		return;
	duckdb_data_chunk_set_size(w->chunk, w->nbuf);
	if (duckdb_append_data_chunk(w->appender, w->chunk) == DuckDBError)
		writer_raise(w, "could not buffer rows for foreign table", duckdb_appender_error(w->appender));
	duckdb_data_chunk_reset(w->chunk);
	w->nbuf = 0;
	refresh_targets(w);
}

/* Everything the appender holds into the buffer table. */
static void
writer_flush(GGWriter *w)
{
	writer_flush_chunk(w);
	if (duckdb_appender_flush(w->appender) == DuckDBError)
		writer_raise(w, "could not buffer rows for foreign table", duckdb_appender_error(w->appender));
}

/* ---------- the transaction's writers ---------- */

static GGWriter *
find_writer(Oid relid)
{
	GGWriter   *w;

	for (w = writers; w != NULL; w = w->next)
		if (w->relid == relid)
			return w;
	return NULL;
}

static bool
is_writer_option(const char *name)
{
	static const char *const names[] = {
		"header", "delim", "quote", "escape", "nullstr", "dateformat",
		"timestampformat", "compression", NULL
	};
	int			i;

	for (i = 0; names[i]; i++)
		if (strcmp(names[i], name) == 0)
			return true;
	return false;
}

/*
 * The writer of a table in this transaction, created on first use: the
 * target, the columns, the buffer table, the appender and the chunk.
 */
static GGWriter *
open_writer(Relation rel, PlanState *ps)
{
	Oid			relid = RelationGetRelid(rel);
	GGWriter   *w = find_writer(relid);
	TupleDesc	desc = RelationGetDescr(rel);
	GGForeignOptions o;
	const char *reason;
	MemoryContext oldcxt;
	StringInfoData ddl;
	ListCell   *lc;
	int			i,
				k;
	int64		kb;

	if (w != NULL)
		return w;

	if (!callbacks_registered)
	{
		RegisterXactCallback(writer_xact, NULL);
		RegisterSubXactCallback(writer_subxact, NULL);
		callbacks_registered = true;
	}
	if (writers_cxt == NULL)
		writers_cxt = AllocSetContextCreate(TopMemoryContext, "gg_duckdb writers", ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(writers_cxt);

	gg_duckdb_foreign_options(relid, &o);
	if (o.catalog)
	{
		/*
		 * One writer at a time: DuckDB's Iceberg commits do not detect each
		 * other (three concurrent inserts left one snapshot and no error),
		 * so a catalog table is written by the coordinator only, and the
		 * table is locked for the transaction.
		 */
		ForeignTable *ft = GetForeignTable(relid);

		if (ft->exec_location != FTEXECLOCATION_COORDINATOR)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gg_duckdb: cannot insert into catalog table \"%s\" from the segments",
							RelationGetRelationName(rel)),
					 errhint("ALTER FOREIGN TABLE %s OPTIONS (ADD mpp_execute 'coordinator'): one writer commits a snapshot.",
							 RelationGetRelationName(rel))));
		LockRelationOid(relid, ExclusiveLock);
	}
	else
		gg_duckdb_check_locations(o.locations);
	w = palloc0(sizeof(GGWriter));
	w->catalog = o.catalog;
	w->relid = relid;
	w->relname = pstrdup(RelationGetRelationName(rel));
	w->format = pstrdup(o.format);
	w->serverid = o.serverid;
	w->target = gg_duckdb_write_target(&o, unique_name(), &reason);
	if (w->target == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: cannot insert into foreign table \"%s\": %s", w->relname, reason)));
	foreach(lc, o.reader_opts)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (is_writer_option(def->defname))
			w->copy_opts = lappend(w->copy_opts, copyObject(def));
	}

	/* the columns: every attribute that is not dropped, by its file name */
	w->attnos = palloc(sizeof(int) * Max(desc->natts, 1));
	w->types = palloc0(sizeof(GGTypeInfo) * Max(desc->natts, 1));
	w->colnames = palloc0(sizeof(char *) * Max(desc->natts, 1));
	k = 0;
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		const char *colname = NameStr(att->attname);
		List	   *colopts;
		ListCell   *oc;

		if (att->attisdropped)
			continue;
		if (!gg_duckdb_type_map(att->atttypid, att->atttypmod, &w->types[k]))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("gg_duckdb: cannot insert into foreign table \"%s\": column \"%s\" has type %s, which DuckDB regions do not carry",
							w->relname, colname, format_type_with_typemod(att->atttypid, att->atttypmod))));
		colopts = GetForeignColumnOptions(relid, i + 1);
		foreach(oc, colopts)
		{
			DefElem    *def = (DefElem *) lfirst(oc);

			if (strcmp(def->defname, "column_name") == 0)
				colname = defGetString(def);
		}
		w->attnos[k] = i + 1;
		w->colnames[k] = gg_duckdb_duck_ident(colname);
		k++;
	}
	w->ncols = k;
	if (k == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: cannot insert into foreign table \"%s\": it has no columns", w->relname)));

	/* the buffer: a table of this backend's instance, under the memory budget */
	gg_duckdb_ensure_s3_secrets(relid, &o);
	w->conn = gg_duckdb_connect();
	if (w->catalog)
		gg_duckdb_iceberg_attach(w->conn, w->serverid);
	/* COPY FROM brings a ModifyTableState without a plan: no operator budget */
	kb = (ps != NULL && ps->plan != NULL) ? (int64) PlanStateOperatorMemKB(ps) : 0;
	if (kb <= 0)
		kb = work_mem;
	kb = Max(kb, (int64) gg_duckdb_min_memory_mb * 1024);
	if (w->catalog)
	{
		/*
		 * DuckDB's Iceberg writer allocates its Parquet row groups at
		 * their default size, about 80 MB at a time, and nothing tunes
		 * that from outside: the budget starts at 256 MB for it.
		 */
		kb = Max(kb, (int64) 256 * 1024);
	}
	kb = Min(kb, (int64) gg_duckdb_max_memory_mb * 1024);
	if (gg_duckdb_reserve_memory)
	{
		MemoryAllocationStatus status = VmemTracker_ReserveVmem(kb * 1024);

		if (status != MemoryAllocation_Success)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("gg_duckdb: could not reserve " INT64_FORMAT " kB for the DuckDB writer of \"%s\" (status %d)",
							kb, w->relname, (int) status)));
		w->memory_reserved = kb * 1024;
	}
	writer_exec(w, psprintf("SET memory_limit = '" INT64_FORMAT "KB'", kb), "could not set the memory limit for foreign table");
	w->budget_kb = kb;

	w->table = psprintf("gg_w_%u", relid);
	initStringInfo(&ddl);
	appendStringInfo(&ddl, "CREATE OR REPLACE TABLE %s (", w->table);
	for (i = 0; i < w->ncols; i++)
		appendStringInfo(&ddl, "%s %s, ", w->colnames[i], gg_duckdb_type_sql(&w->types[i]));
	appendStringInfoString(&ddl, GG_SUB_COLUMN " UINTEGER)");
	writer_exec(w, ddl.data, "could not create the buffer of foreign table");

	w->ltypes = palloc0(sizeof(duckdb_logical_type) * (w->ncols + 1));
	for (i = 0; i < w->ncols; i++)
		w->ltypes[i] = gg_duckdb_logical_type(&w->types[i]);
	w->ltypes[w->ncols] = duckdb_create_logical_type(DUCKDB_TYPE_UINTEGER);
	w->chunk = duckdb_create_data_chunk(w->ltypes, w->ncols + 1);
	if (w->chunk == NULL)
		writer_raise(w, "could not create a data chunk for foreign table", "out of memory");
	w->targets = palloc0(sizeof(GGVectorTarget) * (w->ncols + 1));
	if (duckdb_appender_create(w->conn, NULL, w->table, &w->appender) == DuckDBError)
		writer_raise(w, "could not open the buffer of foreign table",
					 w->appender ? duckdb_appender_error(w->appender) : "appender");
	refresh_targets(w);
	w->max_sub = InvalidSubTransactionId;

	w->next = writers;
	writers = w;
	MemoryContextSwitchTo(oldcxt);
	return w;
}

/* One row into the chunk. */
static void
writer_insert(GGWriter *w, TupleTableSlot *slot)
{
	idx_t		row = w->nbuf;
	SubTransactionId sub = GetCurrentSubTransactionId();
	int			i;

	if (w->poisoned)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("gg_duckdb: the buffer of foreign table \"%s\" is unusable after a failed rollback: %s",
						w->relname, w->poison)));
	slot_getallattrs(slot);
	for (i = 0; i < w->ncols; i++)
	{
		int			att = w->attnos[i] - 1;
		const char *str;
		int			len;

		gg_duckdb_write_datum(&w->types[i], &w->targets[i], row,
							  slot->tts_values[att], slot->tts_isnull[att], &str, &len);
		if (str != NULL)
		{
			duckdb_vector_assign_string_element_len(w->targets[i].vec, row, str, len);
			w->bytes += len + 16;
		}
		else
			w->bytes += 8;
	}
	{
		const char *str;
		int			len;
		GGTypeInfo	ti;

		gg_duckdb_type_map(INT8OID, -1, &ti);
		ti.duck = DUCKDB_TYPE_INTEGER;	/* a 32-bit write, as UINTEGER is */
		gg_duckdb_write_datum(&ti, &w->targets[w->ncols], row, Int32GetDatum((int32) sub), false, &str, &len);
	}
	w->nbuf++;
	w->rows++;
	if (sub > w->max_sub || w->max_sub == InvalidSubTransactionId)
		w->max_sub = sub;
	if (w->nbuf >= duckdb_vector_size())
		writer_flush_chunk(w);
}

/* How many rows the buffer holds. */
static int64
buffered_rows(GGWriter *w)
{
	duckdb_result res;
	int64		n;

	if (duckdb_query(w->conn, psprintf("SELECT count(*) FROM %s", w->table), &res) == DuckDBError)
	{
		const char *msg = duckdb_result_error(&res);
		char	   *copy = pstrdup(msg ? msg : "unknown error");

		duckdb_destroy_result(&res);
		writer_raise(w, "could not count the buffer of foreign table", copy);
	}
	n = duckdb_value_int64(&res, 0, 0);
	duckdb_destroy_result(&res);
	return n;
}

/* The COPY that writes the buffer as the target file, or the INSERT into the catalog table. */
static char *
copy_statement(GGWriter *w)
{
	StringInfoData sql;
	ListCell   *lc;
	int			i;

	initStringInfo(&sql);
	if (w->catalog)
	{
		appendStringInfo(&sql, "INSERT INTO %s (", w->target);
		for (i = 0; i < w->ncols; i++)
			appendStringInfo(&sql, "%s%s", i > 0 ? ", " : "", w->colnames[i]);
		appendStringInfoString(&sql, ") SELECT ");
		for (i = 0; i < w->ncols; i++)
			appendStringInfo(&sql, "%s%s", i > 0 ? ", " : "", w->colnames[i]);
		appendStringInfo(&sql, " FROM %s", w->table);
		return sql.data;
	}
	appendStringInfoString(&sql, "COPY (SELECT ");
	for (i = 0; i < w->ncols; i++)
		appendStringInfo(&sql, "%s%s", i > 0 ? ", " : "", w->colnames[i]);
	appendStringInfo(&sql, " FROM %s) TO %s (FORMAT %s", w->table,
					 gg_duckdb_duck_literal(w->target),
					 strcmp(w->format, "csv") == 0 ? "CSV" :
					 strcmp(w->format, "json") == 0 ? "JSON" : "PARQUET");
	if (strcmp(w->format, "parquet") == 0)
	{
		/*
		 * The Parquet writer holds a whole row group before it writes it,
		 * so a row group takes about an eighth of the memory budget, by
		 * the average width of the buffered rows; DuckDB's default of
		 * 122880 rows stays for rows that fit.
		 */
		int64		width = w->rows > 0 ? Max(w->bytes / w->rows, 8) : 8;	/* over every appended row */
		int64		rows = w->budget_kb * 1024 / 8 / width;

		if (rows < 122880)
			appendStringInfo(&sql, ", ROW_GROUP_SIZE " INT64_FORMAT, Max(rows, 2048));
	}
	foreach(lc, w->copy_opts)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		const char *v = defGetString(def);

		if (strcmp(def->defname, "header") == 0)
		{
			if (strcmp(w->format, "csv") == 0)
				appendStringInfo(&sql, ", HEADER %s", defGetBoolean(def) ? "true" : "false");
		}
		else if (strcmp(def->defname, "delim") == 0)
		{
			if (strcmp(w->format, "csv") == 0)
				appendStringInfo(&sql, ", DELIMITER %s", gg_duckdb_duck_literal(v));
		}
		else if (strcmp(def->defname, "compression") == 0)
			appendStringInfo(&sql, ", COMPRESSION %s", gg_duckdb_duck_literal(v));
		else if (strcmp(w->format, "csv") == 0)
			appendStringInfo(&sql, ", %s %s", def->defname, gg_duckdb_duck_literal(v));
	}
	appendStringInfoChar(&sql, ')');
	return sql.data;
}

/*
 * Write every buffered table: at the end of a transaction that commits
 * here, or prepares here as its part of a distributed transaction (the
 * coordinator's decision after that point is not seen by this process, so
 * a rollback after every segment prepared leaves the files: they carry the
 * distributed transaction id in their names).  An error here aborts the
 * transaction; files already written by it remain.
 */
static void
publish_all(void)
{
	GGWriter   *w;

	for (w = writers; w != NULL; w = w->next)
	{
		if (w->poisoned)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("gg_duckdb: the buffer of foreign table \"%s\" is unusable after a failed rollback: %s",
							w->relname, w->poison)));
		writer_flush(w);
		if (w->appender)
			duckdb_appender_destroy(&w->appender);
		w->appender = NULL;
	}

	/*
	 * The file's row order is nobody's business, and keeping the buffer's
	 * order makes DuckDB hold the batches of a COPY in memory: a buffer
	 * larger than the budget could not be written with it on.  The setting
	 * is the instance's; nothing else of this backend runs a DuckDB query
	 * while its transaction commits.
	 */
	if (writers->conn)
		writer_exec(writers, "SET preserve_insertion_order = false", "could not configure the writer of foreign table");
	PG_TRY();
	{
		for (w = writers; w != NULL; w = w->next)
		{
			/* the buffer's own count: a savepoint may have taken rows out */
			if (buffered_rows(w) == 0)
				continue;
			writer_exec(w, copy_statement(w), w->catalog ? "could not write to the Iceberg table of foreign table"
						: "could not write the file of foreign table");
			gg_duckdb_stats.queries_executed++;
			elog(DEBUG1, "gg_duckdb: writer of \"%s\": wrote %s", w->relname, w->target);
			ereport(DEBUG1,
					(errmsg("gg_duckdb: wrote " INT64_FORMAT " rows of \"%s\" to %s",
							w->rows, w->relname, w->target)));
		}
	}
	PG_CATCH();
	{
		char	   *err;

		if (writers->conn &&
			(err = writer_try(writers, "SET preserve_insertion_order = true")) != NULL)
			pfree(err);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (writers->conn)
		writer_exec(writers, "SET preserve_insertion_order = true", "could not configure the writer of foreign table");
}

static void
writer_xact(XactEvent event, void *arg)
{
	if (writers == NULL)
		return;
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			publish_all();
			release_all();
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PREPARE:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_PARALLEL_ABORT:
			release_all();
			break;
		default:
			break;
	}
}

/*
 * A savepoint that commits hands its rows to its parent; one that rolls
 * back takes them out of the buffer.  Nothing is raised on the abort
 * path: a failure there poisons the writer, and the transaction fails
 * when it tries to write.
 */
static void
writer_subxact(SubXactEvent event, SubTransactionId mySubid,
			   SubTransactionId parentSubid, void *arg)
{
	GGWriter   *w;

	if (writers == NULL)
		return;
	if (event != SUBXACT_EVENT_COMMIT_SUB && event != SUBXACT_EVENT_ABORT_SUB)
		return;
	for (w = writers; w != NULL; w = w->next)
	{
		char	   *sql;
		char	   *err;

		elog(DEBUG1, "gg_duckdb: writer of \"%s\": subtransaction %u %s (parent %u), rows so far " INT64_FORMAT ", newest tag %u",
			 w->relname, mySubid, event == SUBXACT_EVENT_COMMIT_SUB ? "commits" : "aborts",
			 parentSubid, w->rows, w->max_sub);
		if (w->poisoned || w->max_sub == InvalidSubTransactionId || mySubid > w->max_sub)
			continue;
		/* the appender's rows first, so that the statement sees them */
		if (w->nbuf > 0)
		{
			duckdb_data_chunk_set_size(w->chunk, w->nbuf);
			if (duckdb_append_data_chunk(w->appender, w->chunk) == DuckDBError)
			{
				w->poisoned = true;
				w->poison = pstrdup(duckdb_appender_error(w->appender));
				continue;
			}
			duckdb_data_chunk_reset(w->chunk);
			w->nbuf = 0;
			refresh_targets(w);
		}
		if (duckdb_appender_flush(w->appender) == DuckDBError)
		{
			w->poisoned = true;
			w->poison = pstrdup(duckdb_appender_error(w->appender));
			continue;
		}
		if (event == SUBXACT_EVENT_COMMIT_SUB)
			sql = psprintf("UPDATE %s SET " GG_SUB_COLUMN " = %u WHERE " GG_SUB_COLUMN " = %u",
						   w->table, parentSubid, mySubid);
		else
			sql = psprintf("DELETE FROM %s WHERE " GG_SUB_COLUMN " = %u", w->table, mySubid);
		err = writer_try(w, sql);
		if (err != NULL)
		{
			w->poisoned = true;
			w->poison = err;
		}
	}
}

/* ---------- the FDW callbacks ---------- */

int
gg_duckdb_fdw_updatable(Relation rel)
{
	GGForeignOptions o;
	const char *reason;

	gg_duckdb_foreign_options(RelationGetRelid(rel), &o);
	return gg_duckdb_write_target(&o, "x", &reason) != NULL ? (1 << CMD_INSERT) : 0;
}

void
gg_duckdb_fdw_begin_modify(ModifyTableState *mtstate, ResultRelInfo *rinfo,
						   List *fdw_private, int subplan_index, int eflags)
{
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	rinfo->ri_FdwState = open_writer(rinfo->ri_RelationDesc, mtstate ? &mtstate->ps : NULL);
}

void
gg_duckdb_fdw_begin_insert(ModifyTableState *mtstate, ResultRelInfo *rinfo)
{
	rinfo->ri_FdwState = open_writer(rinfo->ri_RelationDesc, mtstate ? &mtstate->ps : NULL);
}

TupleTableSlot *
gg_duckdb_fdw_insert(EState *estate, ResultRelInfo *rinfo,
					 TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	GGWriter   *w = (GGWriter *) rinfo->ri_FdwState;

	if (w == NULL)
		w = open_writer(rinfo->ri_RelationDesc, NULL);
	writer_insert(w, slot);
	return slot;
}

void
gg_duckdb_fdw_end_modify(EState *estate, ResultRelInfo *rinfo)
{
	GGWriter   *w = (GGWriter *) rinfo->ri_FdwState;

	if (w != NULL)
	{
		writer_flush_chunk(w);
		elog(DEBUG1, "gg_duckdb: writer of \"%s\": statement done, rows so far " INT64_FORMAT ", current subtransaction %u",
			 w->relname, w->rows, GetCurrentSubTransactionId());
	}
}

void
gg_duckdb_fdw_end_insert(EState *estate, ResultRelInfo *rinfo)
{
	gg_duckdb_fdw_end_modify(estate, rinfo);
}

void
gg_duckdb_fdw_explain_modify(ModifyTableState *mtstate, ResultRelInfo *rinfo,
							 List *fdw_private, int subplan_index, struct ExplainState *es)
{
	GGForeignOptions o;
	const char *reason;
	char	   *target;

	gg_duckdb_foreign_options(RelationGetRelid(rinfo->ri_RelationDesc), &o);
	target = gg_duckdb_write_target(&o, "gg-<transaction>-<segment>", &reason);
	ExplainPropertyText("DuckDB writer", psprintf("%s %s", o.format, target ? target : reason), es);
}
