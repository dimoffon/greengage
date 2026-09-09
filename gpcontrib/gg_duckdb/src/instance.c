/*-------------------------------------------------------------------------
 *
 * instance.c
 *	  The per-backend embedded DuckDB instance and the task loop that drives
 *	  its queries on the backend thread.
 *
 * The instance is opened lazily by the first caller, in the backend, never in
 * the postmaster.  It is opened with threads=1 and external_threads=1: DuckDB
 * then owns no thread, and every unit of work, table-function callbacks
 * included, runs inside duckdb_pending_execute_task() on this thread.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/memutils.h"

#include "gg_duckdb/gg_duckdb.h"

GGDuckStats gg_duckdb_stats;

static duckdb_database instance = NULL;
static char *instance_temp_dir = NULL;	/* TopMemoryContext */
static int64 instance_memory_limit = 0;
static int	instance_threads = 0;

/* settings applied to the instance so far, to notice GUC changes */
static int	applied_max_memory_mb = -1;
static char *applied_max_temp_size = NULL;

static void
duck_error(const char *what, const char *msg)
{
	gg_duckdb_note_error(msg);
	ereport(ERROR,
			(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			 errmsg("gg_duckdb: %s: %s", what, msg ? msg : "unknown error")));
}

void
gg_duckdb_note_error(const char *msg)
{
	gg_duckdb_stats.errors++;
	if (gg_duckdb_stats.last_error)
		pfree(gg_duckdb_stats.last_error);
	gg_duckdb_stats.last_error =
		msg ? MemoryContextStrdup(TopMemoryContext, msg) : NULL;
}

bool
gg_duckdb_instance_initialized(void)
{
	return instance != NULL;
}

const char *
gg_duckdb_instance_temp_directory(void)
{
	return instance_temp_dir;
}

int64
gg_duckdb_instance_memory_limit_bytes(void)
{
	return instance_memory_limit;
}

int
gg_duckdb_instance_threads(void)
{
	return instance_threads;
}

/*
 * Where this backend spills: a per-pid directory under base/pgsql_tmp, which
 * base backups skip and the postmaster wipes at startup, unless the GUC
 * names another place.
 */
static char *
temp_directory_path(void)
{
	if (gg_duckdb_temp_directory && gg_duckdb_temp_directory[0] != '\0')
		return MemoryContextStrdup(TopMemoryContext, gg_duckdb_temp_directory);

	return MemoryContextStrdup(TopMemoryContext,
							   psprintf("%s/base/%s/%s_gg_duckdb_%d",
										DataDir, PG_TEMP_FILES_DIR,
										PG_TEMP_FILE_PREFIX, MyProcPid));
}

static void
ensure_directory(const char *path)
{
	char	   *parent = pstrdup(path);
	char	   *slash = strrchr(parent, '/');

	if (slash && slash != parent)
	{
		*slash = '\0';
		if (MakePGDirectory(parent) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m", parent)));
	}
	if (MakePGDirectory(path) < 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", path)));
	pfree(parent);
}

static void
set_config(duckdb_config cfg, const char *name, const char *value)
{
	if (duckdb_set_config(cfg, name, value) == DuckDBError)
	{
		duckdb_destroy_config(&cfg);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("gg_duckdb: DuckDB rejected configuration option \"%s\" = \"%s\"",
						name, value)));
	}
}

static void
open_instance(void)
{
	duckdb_config cfg;
	char	   *err = NULL;
	char		buf[64];

	Assert(instance == NULL);

	if (!IsUnderPostmaster && IsPostmasterEnvironment)
		elog(ERROR, "gg_duckdb: the embedded DuckDB may only be opened in a backend");

	instance_temp_dir = temp_directory_path();
	ensure_directory(instance_temp_dir);

	if (duckdb_create_config(&cfg) == DuckDBError)
		elog(ERROR, "gg_duckdb: could not create a DuckDB configuration");

	/* No DuckDB-owned threads: the backend thread drives every task. */
	set_config(cfg, "threads", "1");
	set_config(cfg, "external_threads", "1");
	snprintf(buf, sizeof(buf), "%dMB", gg_duckdb_max_memory_mb);
	set_config(cfg, "memory_limit", buf);
	set_config(cfg, "temp_directory", instance_temp_dir);
	if (gg_duckdb_max_temp_directory_size &&
		gg_duckdb_max_temp_directory_size[0] != '\0')
		set_config(cfg, "max_temp_directory_size",
				   gg_duckdb_max_temp_directory_size);
	/* the backend reads local data; DuckDB itself touches no files but spills */
	set_config(cfg, "enable_external_access", "false");
	set_config(cfg, "autoinstall_known_extensions", "false");
	set_config(cfg, "autoload_known_extensions", "false");

	if (duckdb_open_ext(NULL, &instance, cfg, &err) == DuckDBError)
	{
		char	   *msg = pstrdup(err ? err : "unknown error");

		duckdb_free(err);
		duckdb_destroy_config(&cfg);
		instance = NULL;
		duck_error("could not open the embedded DuckDB", msg);
	}
	duckdb_destroy_config(&cfg);

	/* the leaf table function lives in the instance-wide catalog */
	{
		duckdb_connection conn = NULL;

		if (duckdb_connect(instance, &conn) == DuckDBError)
		{
			duckdb_close(&instance);
			instance = NULL;
			duck_error("could not connect to the embedded DuckDB", NULL);
		}
		PG_TRY();
		{
			gg_duckdb_register_leaf_function(conn);
		}
		PG_CATCH();
		{
			duckdb_disconnect(&conn);
			duckdb_close(&instance);
			instance = NULL;
			PG_RE_THROW();
		}
		PG_END_TRY();
		duckdb_disconnect(&conn);
	}

	instance_threads = 1;
	instance_memory_limit = (int64) gg_duckdb_max_memory_mb * 1024 * 1024;
	applied_max_memory_mb = gg_duckdb_max_memory_mb;
	if (applied_max_temp_size)
		pfree(applied_max_temp_size);
	applied_max_temp_size = MemoryContextStrdup(TopMemoryContext,
												gg_duckdb_max_temp_directory_size ?
												gg_duckdb_max_temp_directory_size : "");

	elog(DEBUG1, "gg_duckdb: opened DuckDB %s (memory_limit %s, temp_directory %s)",
		 duckdb_library_version(), buf, instance_temp_dir);
}

duckdb_database
gg_duckdb_instance(void)
{
	if (instance == NULL)
		open_instance();
	return instance;
}

void
gg_duckdb_instance_close(void)
{
	if (instance == NULL)
		return;
	duckdb_close(&instance);
	instance = NULL;
	instance_memory_limit = 0;
	instance_threads = 0;
	elog(DEBUG1, "gg_duckdb: closed DuckDB");
}

/*
 * Run a statement on a connection and require success; used for settings.
 */
static void
run_setting(duckdb_connection conn, const char *sql)
{
	duckdb_result res;

	if (duckdb_query(conn, sql, &res) == DuckDBError)
	{
		char	   *msg = pstrdup(duckdb_result_error(&res) ?
								 duckdb_result_error(&res) : "unknown error");

		duckdb_destroy_result(&res);
		duck_error(sql, msg);
	}
	duckdb_destroy_result(&res);
}

/*
 * Apply GUCs that changed since they were last applied.  DuckDB accepts these
 * at run time, so nothing is locked once the instance exists.
 */
static void
apply_settings(duckdb_connection conn)
{
	const char *max_temp = gg_duckdb_max_temp_directory_size ?
	gg_duckdb_max_temp_directory_size : "";

	if (applied_max_memory_mb != gg_duckdb_max_memory_mb)
	{
		char	   *sql = psprintf("SET GLOBAL memory_limit = '%dMB'",
								   gg_duckdb_max_memory_mb);

		run_setting(conn, sql);
		pfree(sql);
		applied_max_memory_mb = gg_duckdb_max_memory_mb;
		instance_memory_limit = (int64) gg_duckdb_max_memory_mb * 1024 * 1024;
	}
	if (strcmp(applied_max_temp_size ? applied_max_temp_size : "", max_temp) != 0)
	{
		if (max_temp[0] != '\0')
		{
			char	   *sql = psprintf("SET GLOBAL max_temp_directory_size = '%s'",
									   max_temp);

			run_setting(conn, sql);
			pfree(sql);
		}
		else
			run_setting(conn, "RESET GLOBAL max_temp_directory_size");
		if (applied_max_temp_size)
			pfree(applied_max_temp_size);
		applied_max_temp_size = MemoryContextStrdup(TopMemoryContext, max_temp);
	}
}

duckdb_connection
gg_duckdb_connect(void)
{
	duckdb_database db = gg_duckdb_instance();
	duckdb_connection conn = NULL;

	if (duckdb_connect(db, &conn) == DuckDBError)
		duck_error("could not connect to the embedded DuckDB", NULL);

	PG_TRY();
	{
		apply_settings(conn);
	}
	PG_CATCH();
	{
		duckdb_disconnect(&conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return conn;
}

void
gg_duckdb_disconnect(duckdb_connection *conn)
{
	if (*conn)
		duckdb_disconnect(conn);
	*conn = NULL;
}

/*
 * Drive a pending result until it is ready.  Cancel requests are honoured
 * between tasks: the query is interrupted and the usual PG error is raised,
 * which the caller's PG_CATCH turns into freed DuckDB objects.
 */
void
gg_duckdb_run_pending(duckdb_connection conn, duckdb_pending_result pending)
{
	int64		tasks = 0;
	int			idle = 0;

	for (;;)
	{
		duckdb_pending_state state;

		if (InterruptPending)
		{
			duckdb_interrupt(conn);
			CHECK_FOR_INTERRUPTS();
		}

		state = duckdb_pending_execute_task(pending);
		tasks++;

		if (state == DUCKDB_PENDING_RESULT_READY)
			break;
		if (state == DUCKDB_PENDING_ERROR)
		{
			const char *msg = duckdb_pending_error(pending);

			if (InterruptPending)
				CHECK_FOR_INTERRUPTS();	/* it was our interrupt */
			duck_error("query failed", msg ? pstrdup(msg) : NULL);
		}
		if (state == DUCKDB_PENDING_NO_TASKS_AVAILABLE)
		{
			/*
			 * With no worker threads nothing else can make progress; this
			 * state is expected to be transient.  Avoid burning a core and
			 * give up if it persists.
			 */
			if (++idle > 10000)
				duck_error("query failed", "DuckDB executor made no progress");
			pg_usleep(100);
		}
		else
			idle = 0;
	}

	elog(DEBUG2, "gg_duckdb: pending result ready after " INT64_FORMAT " task(s)", tasks);
}
