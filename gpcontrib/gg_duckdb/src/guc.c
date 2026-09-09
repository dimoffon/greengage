/*-------------------------------------------------------------------------
 *
 * guc.c
 *	  gg_duckdb.* configuration parameters.
 *
 * Runtime parameters that a QE must see carry GUC_GPDB_NEED_SYNC: the
 * dispatcher hands such GUCs to every newly started gang as -c options, and
 * SET reaches gangs that already exist.  Extension GUCs get GUC_GPDB_NO_SYNC
 * by default, which would leave the segments with the defaults.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "utils/guc.h"

#include "gg_duckdb/gg_duckdb.h"

int			gg_duckdb_mode = GG_DUCKDB_MODE_OFF;
int			gg_duckdb_max_memory_mb = 512;
int			gg_duckdb_min_memory_mb = 64;
char	   *gg_duckdb_temp_directory = NULL;
char	   *gg_duckdb_max_temp_directory_size = NULL;
bool		gg_duckdb_release_instance_at_end = false;
int			gg_duckdb_debug_wrap = GG_DUCKDB_WRAP_OFF;
char	   *gg_duckdb_debug_region_sql = NULL;
int			gg_duckdb_min_rows = 100000;
bool		gg_duckdb_explain_decisions = false;
bool		gg_duckdb_validate_at_plan_time = true;
bool		gg_duckdb_on_coordinator = false;
bool		gg_duckdb_reserve_memory = true;
bool		gg_duckdb_strict = false;
char	   *gg_duckdb_data_directories = NULL;
char	   *gg_duckdb_http_proxy = NULL;
double		gg_duckdb_cost_fixed = 50.0;
double		gg_duckdb_cost_convert_row = 0.001;
double		gg_duckdb_cost_convert_byte = 0.0003;
double		gg_duckdb_cost_op_factor = 0.25;
double		gg_duckdb_cost_margin = 0.25;

static const struct config_enum_entry gg_duckdb_debug_wrap_options[] =
{
	{"off", GG_DUCKDB_WRAP_OFF, false},
	{"scans", GG_DUCKDB_WRAP_SCANS, false},
	{NULL, 0, false}
};

static const struct config_enum_entry gg_duckdb_mode_options[] =
{
	{"off", GG_DUCKDB_MODE_OFF, false},
	{"auto", GG_DUCKDB_MODE_AUTO, false},
	{"force", GG_DUCKDB_MODE_FORCE, false},
	{NULL, 0, false}
};

void
gg_duckdb_define_gucs(void)
{
	DefineCustomEnumVariable("gg_duckdb.mode",
							 "Whether plans may hand local subtrees to DuckDB.",
							 "off: never; auto: when the cost gate says so; force: every eligible subtree.",
							 &gg_duckdb_mode,
							 GG_DUCKDB_MODE_OFF,
							 gg_duckdb_mode_options,
							 PGC_USERSET,
							 GUC_GPDB_NEED_SYNC,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("gg_duckdb.max_memory",
							"Upper bound of the embedded DuckDB's memory limit, per backend.",
							"The per-query budget derived from statement_mem or the resource group is capped by this.",
							&gg_duckdb_max_memory_mb,
							512, 16, INT_MAX / 1024,
							PGC_SUSET,
							GUC_UNIT_MB | GUC_GPDB_NEED_SYNC,
							NULL, NULL, NULL);

	DefineCustomIntVariable("gg_duckdb.min_memory",
							"Lower bound of a region's DuckDB memory limit.",
							"A region's share of the query memory is raised to this: DuckDB needs room for its buffers even when memquota grants a node next to nothing.",
							&gg_duckdb_min_memory_mb,
							64, 8, INT_MAX / 1024,
							PGC_USERSET,
							GUC_UNIT_MB | GUC_GPDB_NEED_SYNC,
							NULL, NULL, NULL);

	DefineCustomStringVariable("gg_duckdb.temp_directory",
							   "Directory for DuckDB spill files.",
							   "Empty means base/pgsql_tmp/pgsql_tmp_gg_duckdb_<pid> under the data directory of each node.",
							   &gg_duckdb_temp_directory,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("gg_duckdb.max_temp_directory_size",
							   "Maximum size of DuckDB spill files, as a DuckDB size string such as '10GB'.",
							   "Empty leaves DuckDB's default (most of the free space on the volume).",
							   &gg_duckdb_max_temp_directory_size,
							   "",
							   PGC_SUSET,
							   GUC_GPDB_NEED_SYNC,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("gg_duckdb.release_instance_at_end",
							 "Close the embedded DuckDB after each statement instead of keeping it for the session.",
							 NULL,
							 &gg_duckdb_release_instance_at_end,
							 false,
							 PGC_USERSET,
							 GUC_GPDB_NEED_SYNC,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("gg_duckdb.debug_wrap",
							 "Development aid: wrap plan nodes into identity DuckDB regions.",
							 "scans: every SeqScan of a segment slice becomes SELECT c1..cN FROM gg_leaf(0).",
							 &gg_duckdb_debug_wrap,
							 GG_DUCKDB_WRAP_OFF,
							 gg_duckdb_debug_wrap_options,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("gg_duckdb.debug_region_sql",
							   "Development aid: DuckDB query used by debug_wrap regions instead of the identity.",
							   NULL,
							   &gg_duckdb_debug_region_sql,
							   "",
							   PGC_USERSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomIntVariable("gg_duckdb.min_rows",
							"Estimated rows a region must take in before mode=auto uses DuckDB for it.",
							NULL,
							&gg_duckdb_min_rows,
							100000, 0, INT_MAX,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("gg_duckdb.explain_decisions",
							 "Report, as NOTICEs at plan time, every subtree considered for DuckDB and why it was rejected.",
							 NULL,
							 &gg_duckdb_explain_decisions,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("gg_duckdb.validate_at_plan_time",
							 "Prepare every region query on the coordinator's DuckDB before using it; a subtree whose query does not bind stays on the standard executor.",
							 NULL,
							 &gg_duckdb_validate_at_plan_time,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("gg_duckdb.on_coordinator",
							 "Allow regions in slices that run on the coordinator.",
							 NULL,
							 &gg_duckdb_on_coordinator,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("gg_duckdb.reserve_memory",
							 "Reserve a region's memory budget with the vmem tracker while it runs.",
							 NULL,
							 &gg_duckdb_reserve_memory,
							 true,
							 PGC_USERSET,
							 GUC_GPDB_NEED_SYNC,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("gg_duckdb.strict",
							 "Raise an error instead of a notice when a DuckDB() hint cannot be honoured.",
							 NULL,
							 &gg_duckdb_strict,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomRealVariable("gg_duckdb.cost_fixed",
							 "Cost gate: fixed cost of a region (preparing and starting a DuckDB query), in planner cost units.",
							 NULL,
							 &gg_duckdb_cost_fixed,
							 50.0, 0.0, 1e9,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomRealVariable("gg_duckdb.cost_convert_row",
							 "Cost gate: cost of converting one row into or out of DuckDB, in planner cost units.",
							 NULL,
							 &gg_duckdb_cost_convert_row,
							 0.001, 0.0, 1e9,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomRealVariable("gg_duckdb.cost_convert_byte",
							 "Cost gate: cost of converting one byte into or out of DuckDB, in planner cost units.",
							 NULL,
							 &gg_duckdb_cost_convert_byte,
							 0.0003, 0.0, 1e9,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomRealVariable("gg_duckdb.cost_op_factor",
							 "Cost gate: DuckDB's cost of an interior operator relative to the standard executor's.",
							 NULL,
							 &gg_duckdb_cost_op_factor,
							 0.25, 0.0, 1e9,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomRealVariable("gg_duckdb.cost_margin",
							 "Cost gate: DuckDB's estimate must beat the standard executor's by this fraction.",
							 NULL,
							 &gg_duckdb_cost_margin,
							 0.25, 0.0, 1e9,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("gg_duckdb.data_directories",
							   "Directories DuckDB may read files from (comma-separated absolute paths); empty allows none.",
							   "The foreign data wrapper reads Parquet, CSV and JSON files under these directories on every node. DuckDB's own file access stays disabled outside them.",
							   &gg_duckdb_data_directories,
							   "",
							   PGC_SUSET,
							   GUC_GPDB_NEED_SYNC,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("gg_duckdb.http_proxy",
							   "HTTP proxy (host:port) DuckDB uses for remote files; empty for none.",
							   "The environment's http_proxy is not used: a database server rarely wants it, and DuckDB rejects some of its spellings.",
							   &gg_duckdb_http_proxy,
							   "",
							   PGC_SUSET,
							   GUC_GPDB_NEED_SYNC,
							   NULL, NULL, NULL);

	EmitWarningsOnPlaceholders("gg_duckdb");
}
