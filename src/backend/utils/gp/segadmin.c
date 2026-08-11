/*-------------------------------------------------------------------------
 *
 * segadmin.c
 *	  Functions to support administrative tasks with GPDB segments.
 *
 * Portions Copyright (c) 2010 Greenplum
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/backend/utils/gp/segadmin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq-fe.h"
#include "miscadmin.h"
#include "pqexpbuffer.h"

#include "catalog/gp_segment_configuration.h"
#include "catalog/pg_proc.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbtopology.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbfts.h"
#include "postmaster/startup.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#define COORDINATOR_ONLY 0x1
#define UTILITY_MODE 0x2
#define SUPERUSER 0x4
#define READ_ONLY 0x8
#define SEGMENT_ONLY 0x10
#define STANDBY_ONLY 0x20
#define SINGLE_USER_MODE 0x40

/*
 * Everything below reads and writes the cluster topology through a write set
 * (cdbtopology.h) rather than through gp_segment_configuration directly.  The
 * catalog is still where it lands -- that is the provider's business -- but
 * the questions these functions ask and the changes they make are answered
 * from, and applied to, one snapshot taken under one lock.
 *
 * That is also why the helpers below take the write set: a check that reads
 * the catalog while the mutation edits an array is a check of something else.
 */

/* look up a particular segment */
static GpSegConfigEntry *
get_segconfig(GpTopoWriteSet *ws, int16 dbid)
{
	GpSegConfigEntry *config = GpTopoFindByDbid(ws, dbid);

	if (config == NULL)
		elog(ERROR, "could not find configuration entry for dbid %i", dbid);

	return config;
}

/* Convenience routine to look up the primary for a given segment index */
static int16
content_get_primary_dbid(GpTopoWriteSet *ws, int16 contentid)
{
	GpSegConfigEntry *config;

	config = GpTopoFindByContentRole(ws, contentid,
									 GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY,
									 false /* false == current, not preferred,
											* role */ );

	return config ? config->dbid : 0;
}

/* Convenience routine to look up the mirror for a given segment index */
static int16
content_get_mirror_dbid(GpTopoWriteSet *ws, int16 contentid)
{
	GpSegConfigEntry *config;

	config = GpTopoFindByContentRole(ws, contentid,
									 GP_SEGMENT_CONFIGURATION_ROLE_MIRROR,
									 false /* false == current, not preferred,
											* role */ );

	return config ? config->dbid : 0;
}

/* Tell the caller whether a mirror exists at a given segment index */
static bool
segment_has_mirror(GpTopoWriteSet *ws, int16 contentid)
{
	return content_get_mirror_dbid(ws, contentid) != 0;
}

/*
 * As the function name says, test whether a given dbid is the dbid of the
 * standby coordinator.
 */
static bool
dbid_is_coordinator_standby(GpTopoWriteSet *ws, int16 dbid)
{
	int16		standbydbid = content_get_mirror_dbid(ws, COORDINATOR_CONTENT_ID);

	return (standbydbid == dbid);
}

/*
 * Tell the caller whether a standby coordinator is defined in the system.
 */
static bool
standby_exists(GpTopoWriteSet *ws)
{
	return segment_has_mirror(ws, COORDINATOR_CONTENT_ID);
}

/*
 * Check that the code is being called in right context.
 *
 * `ws` may be NULL unless STANDBY_ONLY is asked for; that is the one check
 * here that needs to know what the topology says.
 */
static void
mirroring_sanity_check(GpTopoWriteSet *ws, int flags, const char *func)
{
	if ((flags & COORDINATOR_ONLY) == COORDINATOR_ONLY)
	{
		if (GpIdentity.dbid == UNINITIALIZED_GP_IDENTITY_VALUE)
			elog(ERROR, "%s requires valid GpIdentity dbid", func);

		if (!IS_QUERY_DISPATCHER())
			elog(ERROR, "%s must be run on the coordinator", func);
	}

	if ((flags & UTILITY_MODE) == UTILITY_MODE)
	{
		if (Gp_role != GP_ROLE_UTILITY)
			elog(ERROR, "%s must be run in utility mode", func);
	}

	if ((flags & SINGLE_USER_MODE) == SINGLE_USER_MODE)
	{
		if (IsUnderPostmaster)
			elog(ERROR, "%s must be run in single-user mode", func);
	}

	if ((flags & SUPERUSER) == SUPERUSER)
	{
		if (!superuser())
			elog(ERROR, "%s can only be run by a superuser", func);
	}

	if ((flags & SEGMENT_ONLY) == SEGMENT_ONLY)
	{
		if (GpIdentity.dbid == UNINITIALIZED_GP_IDENTITY_VALUE)
			elog(ERROR, "%s requires valid GpIdentity dbid", func);

		if (IS_QUERY_DISPATCHER())
			elog(ERROR, "%s cannot be run on the coordinator", func);
	}

	if ((flags & STANDBY_ONLY) == STANDBY_ONLY)
	{
		if (GpIdentity.dbid == UNINITIALIZED_GP_IDENTITY_VALUE)
			elog(ERROR, "%s requires valid GpIdentity dbid", func);

		Assert(ws != NULL);
		if (!dbid_is_coordinator_standby(ws, GpIdentity.dbid))
			elog(ERROR, "%s can only be run on the standby coordinator", func);
	}
}

static void
add_segment(GpTopoWriteSet *ws, GpSegConfigEntry *new_segment_information)
{
	if (new_segment_information->role == GP_SEGMENT_CONFIGURATION_ROLE_MIRROR)
	{
		int16		primary_dbid;
		GpSegConfigEntry *preferred_primary;

		primary_dbid = content_get_primary_dbid(ws,
												new_segment_information->segindex);
		if (!primary_dbid)
			elog(ERROR, "contentid %i does not point to an existing segment",
				 new_segment_information->segindex);

		/*
		 * no mirrors should be defined
		 */
		if (segment_has_mirror(ws, new_segment_information->segindex))
			elog(ERROR, "segment already has a mirror defined");

		/*
		 * figure out if the preferred role of this mirror needs to be primary
		 * or mirror (no preferred primary -- make this one the preferred
		 * primary)
		 */
		preferred_primary = GpTopoFindByContentRole(ws,
													new_segment_information->segindex,
													GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY,
													true /* preferred role */ );

		if (preferred_primary == NULL && new_segment_information->preferred_role == GP_SEGMENT_CONFIGURATION_ROLE_MIRROR)
		{
			elog(NOTICE, "override preferred_role of this mirror as primary to support rebalance operation.");
			new_segment_information->preferred_role = GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY;
		}
	}

	GpTopoInsert(ws, new_segment_information);
}

/*
 * Tell the coordinator about a new primary segment.
 *
 * gp_add_segment_primary(hostname, address, port)
 *
 * Args:
 *   hostname - host name string
 *   address - either hostname or something else
 *   port - port number
 *   datadir - absolute path to primary data directory.
 *
 * Returns the dbid of the new segment.
 */
Datum
gp_add_segment_primary(PG_FUNCTION_ARGS)
{
	GpSegConfigEntry	new;
	GpTopoWriteSet	   *ws;

	MemSet(&new, 0, sizeof(GpSegConfigEntry));

	if (PG_ARGISNULL(0))
		elog(ERROR, "hostname cannot be NULL");
	new.hostname = TextDatumGetCString(PG_GETARG_DATUM(0));

	if (PG_ARGISNULL(1))
		elog(ERROR, "address cannot be NULL");
	new.address = TextDatumGetCString(PG_GETARG_DATUM(1));

	if (PG_ARGISNULL(2))
		elog(ERROR, "port cannot be NULL");
	new.port = PG_GETARG_INT32(2);

	if (PG_ARGISNULL(3))
		elog(ERROR, "datadir cannot be NULL");
	new.datadir = TextDatumGetCString(PG_GETARG_DATUM(3));

	mirroring_sanity_check(NULL, COORDINATOR_ONLY | SUPERUSER,
						   "gp_add_segment_primary");

	ws = GpTopoBeginWrite(CurTransactionContext, GP_TOPO_WRITE_SERIALIZED);

	/*
	 * The content id is derived from what is already there, so a topology with
	 * no coordinator in it would hand out content 0 to a segment of a cluster
	 * that does not exist yet.  Nothing above this point notices: the checks
	 * read GpIdentity, which comes from the postmaster's command line.
	 */
	if (GpTopoFindByContentRole(ws, COORDINATOR_CONTENT_ID,
								GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY,
								true) == NULL)
		elog(ERROR, "gp_add_segment_primary requires a coordinator entry in the cluster topology");

	new.segindex = GpTopoNextContent(ws);
	new.dbid = GpTopoAvailableDbid(ws);
	new.role = GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY;
	new.preferred_role = GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY;
	new.mode = GP_SEGMENT_CONFIGURATION_MODE_NOTINSYNC;
	new.status = GP_SEGMENT_CONFIGURATION_STATUS_UP;

	add_segment(ws, &new);

	GpTopoCommitWrite(ws);

	PG_RETURN_INT16(new.dbid);
}

/*
 * Currently this is called by `gpinitsystem`.
 *
 * This method shouldn't be called at all. `gp_add_segment_primary()` and
 * `gp_add_segment_mirror()` should be used instead. This is to avoid setting
 * character values of role, preferred_role, mode, status, etc. outside database.
 */
Datum
gp_add_segment(PG_FUNCTION_ARGS)
{
	GpSegConfigEntry new;
	GpTopoWriteSet *ws;

	MemSet(&new, 0, sizeof(GpSegConfigEntry));

	if (PG_ARGISNULL(0))
		elog(ERROR, "dbid cannot be NULL");
	new.dbid = PG_GETARG_INT16(0);

	if (PG_ARGISNULL(1))
		elog(ERROR, "content cannot be NULL");
	new.segindex = PG_GETARG_INT16(1);

	if (PG_ARGISNULL(2))
		elog(ERROR, "role cannot be NULL");
	new.role = PG_GETARG_CHAR(2);

	if (PG_ARGISNULL(3))
		elog(ERROR, "preferred_role cannot be NULL");
	new.preferred_role = PG_GETARG_CHAR(3);

	if (PG_ARGISNULL(4))
		elog(ERROR, "mode cannot be NULL");
	new.mode = PG_GETARG_CHAR(4);

	if (PG_ARGISNULL(5))
		elog(ERROR, "status cannot be NULL");
	new.status = PG_GETARG_CHAR(5);

	if (PG_ARGISNULL(6))
		elog(ERROR, "port cannot be NULL");
	new.port = PG_GETARG_INT32(6);

	if (PG_ARGISNULL(7))
		elog(ERROR, "hostname cannot be NULL");
	new.hostname = TextDatumGetCString(PG_GETARG_DATUM(7));

	if (PG_ARGISNULL(8))
		elog(ERROR, "address cannot be NULL");
	new.address = TextDatumGetCString(PG_GETARG_DATUM(8));

	if (PG_ARGISNULL(9))
		elog(ERROR, "datadir cannot be NULL");
	new.datadir = TextDatumGetCString(PG_GETARG_DATUM(9));

	mirroring_sanity_check(NULL, COORDINATOR_ONLY | SUPERUSER, "gp_add_segment");

	new.mode = GP_SEGMENT_CONFIGURATION_MODE_NOTINSYNC;
	elog(NOTICE, "mode is changed to GP_SEGMENT_CONFIGURATION_MODE_NOTINSYNC under walrep.");

	ws = GpTopoBeginWrite(CurTransactionContext, GP_TOPO_WRITE_SERIALIZED);

	add_segment(ws, &new);

	GpTopoCommitWrite(ws);

	PG_RETURN_INT16(new.dbid);
}

/*
 * Coordinator function to remove a segment from all catalogs
 */
static void
remove_segment(GpTopoWriteSet *ws, int16 dbid)
{
	int			numDel PG_USED_FOR_ASSERTS_ONLY;

	/* Check that the segment exists at all */
	get_segconfig(ws, dbid);

	numDel = GpTopoDelete(ws, dbid);
	Assert(numDel > 0);
}

/*
 * Remove knowledge of a segment from the coordinator.
 *
 * gp_remove_segment(dbid)
 *
 * Args:
 *   dbid - db identifier
 *
 * Returns:
 *   true on success, otherwise error.
 */
Datum
gp_remove_segment(PG_FUNCTION_ARGS)
{
	int16		dbid;
	GpTopoWriteSet *ws;

	if (PG_ARGISNULL(0))
		elog(ERROR, "dbid cannot be NULL");

	dbid = PG_GETARG_INT16(0);

	mirroring_sanity_check(NULL, COORDINATOR_ONLY | SUPERUSER | UTILITY_MODE,
						   "gp_remove_segment");

	/*
	 * Stronger than this path used to take: it topped out at RowExclusiveLock.
	 * See GpTopoWriteLevel -- ROW is not the old behaviour either, and reading
	 * the topology and then deleting from it wants the whole-set lock.
	 */
	ws = GpTopoBeginWrite(CurTransactionContext, GP_TOPO_WRITE_SERIALIZED);

	remove_segment(ws, dbid);

	GpTopoCommitWrite(ws);

	PG_RETURN_BOOL(true);
}

/*
 * Add a mirror of an existing segment.
 *
 * gp_add_segment_mirror(contentid, hostname, address, port, datadir)
 */
Datum
gp_add_segment_mirror(PG_FUNCTION_ARGS)
{
	GpSegConfigEntry new;
	GpTopoWriteSet *ws;

	MemSet(&new, 0, sizeof(GpSegConfigEntry));

	if (PG_ARGISNULL(0))
		elog(ERROR, "contentid cannot be NULL");
	new.segindex = PG_GETARG_INT16(0);

	if (PG_ARGISNULL(1))
		elog(ERROR, "hostname cannot be NULL");
	new.hostname = TextDatumGetCString(PG_GETARG_DATUM(1));

	if (PG_ARGISNULL(2))
		elog(ERROR, "address cannot be NULL");
	new.address = TextDatumGetCString(PG_GETARG_DATUM(2));

	if (PG_ARGISNULL(3))
		elog(ERROR, "port cannot be NULL");
	new.port = PG_GETARG_INT32(3);

	if (PG_ARGISNULL(4))
		elog(ERROR, "datadir cannot be NULL");
	new.datadir = TextDatumGetCString(PG_GETARG_DATUM(4));
	
	mirroring_sanity_check(NULL, COORDINATOR_ONLY | SUPERUSER,
						   "gp_add_segment_mirror");

	ws = GpTopoBeginWrite(CurTransactionContext, GP_TOPO_WRITE_SERIALIZED);

	new.dbid = GpTopoAvailableDbid(ws);
	new.mode = GP_SEGMENT_CONFIGURATION_MODE_NOTINSYNC;
	new.status = GP_SEGMENT_CONFIGURATION_STATUS_DOWN;
	new.role = GP_SEGMENT_CONFIGURATION_ROLE_MIRROR;
	new.preferred_role = GP_SEGMENT_CONFIGURATION_ROLE_MIRROR;

	add_segment(ws, &new);

	GpTopoCommitWrite(ws);

	PG_RETURN_INT16(new.dbid);
}

/*
 * Remove a segment mirror.
 *
 * gp_remove_segment_mirror(contentid)
 *
 * Args:
 *   contentid - segment index at which to remove the mirror
 *
 * Returns:
 *   true upon success, otherwise throws error.
 */
Datum
gp_remove_segment_mirror(PG_FUNCTION_ARGS)
{
	int16		contentid = 0;
	int16		pridbid;
	int16		mirdbid;
	GpTopoWriteSet *ws;

	if (PG_ARGISNULL(0))
		elog(ERROR, "dbid cannot be NULL");
	contentid = PG_GETARG_INT16(0);

	mirroring_sanity_check(NULL, COORDINATOR_ONLY | SUPERUSER,
						   "gp_remove_segment_mirror");

	/* the write set's lock is what avoids races here */
	ws = GpTopoBeginWrite(CurTransactionContext, GP_TOPO_WRITE_SERIALIZED);

	pridbid = content_get_primary_dbid(ws, contentid);

	if (!pridbid)
		elog(ERROR, "no dbid for contentid %i", contentid);

	if (!segment_has_mirror(ws, contentid))
		elog(ERROR, "segment does not have a mirror");

	mirdbid = content_get_mirror_dbid(ws, contentid);
	if (!mirdbid)
		elog(ERROR, "no mirror dbid for contentid %i", contentid);

	remove_segment(ws, mirdbid);

	GpTopoCommitWrite(ws);

	PG_RETURN_BOOL(true);
}

/*
 * Add a coordinator standby.
 *
 * gp_add_coordinator_standby(hostname, address, [port])
 *
 * Args:
 *  hostname - as above
 *  address - as above
 *  port - the port number of new standby
 *
 * Returns:
 *  dbid of the new standby
 */
Datum
gp_add_coordinator_standby_port(PG_FUNCTION_ARGS)
{
	return gp_add_coordinator_standby(fcinfo);
}

Datum
gp_add_coordinator_standby(PG_FUNCTION_ARGS)
{
	int16		coordinator_dbid;
	GpTopoWriteSet *ws;
	GpSegConfigEntry standby;

	if (PG_ARGISNULL(0))
		elog(ERROR, "host name cannot be NULL");
	if (PG_ARGISNULL(1))
		elog(ERROR, "address cannot be NULL");
	if (PG_ARGISNULL(2))
		elog(ERROR, "datadir cannot be NULL");

	mirroring_sanity_check(NULL, COORDINATOR_ONLY | UTILITY_MODE,
						   "gp_add_coordinator_standby");

	/*
	 * A standby coordinator learns the topology only because the coordinator's
	 * writes reach it, and for a store that is not WAL-logged nothing carries
	 * them.  Refuse rather than hand back a standby whose topology silently
	 * goes stale.  gpinitstandby inherits this for free.
	 */
	if (!GpTopoActiveProvider()->allows_standby_coordinator)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a standby coordinator cannot be added while the cluster "
						"topology is stored by the \"%s\" provider",
						GpTopoActiveProvider()->name),
				 errdetail("That store is not WAL-logged, so the standby's copy "
						   "of the topology could not be kept in step."),
				 errhint("Use gp_topology_source = catalog, or an external "
						 "topology store.")));

	/*
	 * Open the write set before checking whether a standby already exists.
	 * The check used to run outside the lock that the insert then took, so two
	 * callers could both find no standby and both add one.
	 */
	ws = GpTopoBeginWrite(CurTransactionContext, GP_TOPO_WRITE_SERIALIZED);

	/* Check if the system is ok */
	if (standby_exists(ws))
		elog(ERROR, "only a single coordinator standby may be defined");

	/*
	 * Don't reference GpIdentity.dbid, as it is legitimate to set -1 for -b
	 * option in utility mode.  Content ID = -1 AND role =
	 * GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY is the definition of primary
	 * coordinator.
	 */
	coordinator_dbid = content_get_primary_dbid(ws, COORDINATOR_CONTENT_ID);

	/*
	 * The standby starts as a copy of the coordinator's entry -- by value.
	 * Editing the coordinator's own entry in place would make the diff rewrite
	 * the coordinator's row as well as adding the standby's.
	 */
	standby = *get_segconfig(ws, coordinator_dbid);

	standby.dbid = GpTopoMaxDbid(ws) + 1;
	standby.role = GP_SEGMENT_CONFIGURATION_ROLE_MIRROR;
	standby.preferred_role = GP_SEGMENT_CONFIGURATION_ROLE_MIRROR;
	standby.mode = GP_SEGMENT_CONFIGURATION_MODE_INSYNC;
	standby.status = GP_SEGMENT_CONFIGURATION_STATUS_UP;

	standby.hostname = TextDatumGetCString(PG_GETARG_TEXT_P(0));

	standby.address = TextDatumGetCString(PG_GETARG_TEXT_P(1));

	standby.datadir = TextDatumGetCString(PG_GETARG_TEXT_P(2));

	/* Use the new port number if specified */
	if (PG_NARGS() > 3 && !PG_ARGISNULL(3))
		standby.port = PG_GETARG_INT32(3);

	GpTopoInsert(ws, &standby);

	GpTopoCommitWrite(ws);

	PG_RETURN_INT16(standby.dbid);
}

/*
 * Remove the coordinator standby.
 *
 * gp_remove_coordinator_standby()
 *
 * Returns:
 *  true upon success otherwise false
 */
Datum
gp_remove_coordinator_standby(PG_FUNCTION_ARGS)
{
	int16		dbid;
	GpTopoWriteSet *ws;

	mirroring_sanity_check(NULL, SUPERUSER | COORDINATOR_ONLY | UTILITY_MODE,
						   "gp_remove_coordinator_standby");

	/* Stronger than this path used to take; see gp_remove_segment(). */
	ws = GpTopoBeginWrite(CurTransactionContext, GP_TOPO_WRITE_SERIALIZED);

	dbid = content_get_mirror_dbid(ws, COORDINATOR_CONTENT_ID);

	if (!dbid)
		elog(ERROR, "no coordinator standby defined");

	remove_segment(ws, dbid);

	GpTopoCommitWrite(ws);

	PG_RETURN_BOOL(true);
}

static void
segment_config_activate_standby(GpTopoWriteSet *ws, int16 standby_dbid,
								int16 coordinator_dbid)
{
	GpSegConfigEntry *standby;

	/* first, delete the old coordinator */
	if (GpTopoDelete(ws, coordinator_dbid) == 0)
		elog(ERROR, "cannot find old coordinator, dbid %i", coordinator_dbid);

	/* now, set out rows for old standby. */
	standby = GpTopoFindByDbid(ws, standby_dbid);

	if (standby == NULL)
		elog(ERROR, "cannot find standby, dbid %i", standby_dbid);

	/* old standby keeps its previous dbid. */
	standby->role = GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY;
	standby->preferred_role = GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY;
	GpTopoUpdate(ws, standby);
}

/*
 * Activate a standby. To do this, we need to update gp_segment_configuration.
 *
 * Returns:
 *  true upon success, otherwise throws error.
 */
bool
gp_activate_standby(void)
{
	int16		standby_dbid = GpIdentity.dbid;
	int16		coordinator_dbid;
	GpTopoWriteSet *ws;
	GpSegConfigEntry *coordinator;

	/*
	 * Runs in the startup process on the promotion path, where the write set
	 * is the whole of what this function may touch: no dispatcher, no FTS, and
	 * a relcache with only what UpdateCatalogForStandbyPromotion()'s
	 * transaction has opened.
	 */
	ws = GpTopoBeginWrite(CurTransactionContext, GP_TOPO_WRITE_SERIALIZED);

	coordinator = GpTopoFindByContentRole(ws, COORDINATOR_CONTENT_ID,
										  GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY,
										  true);
	coordinator_dbid = coordinator ? coordinator->dbid : 0;

	/*
	 * This call comes from Startup process post checking state in pg_control
	 * file to make sure its standby. If user calls (ideally SHOULD NOT) but
	 * just for troubleshooting or wired case must make usre its only executed
	 * on Standby. So, this checking should return after matching DBIDs only
	 * for StartUp Process, to cover for case of crash after updating the
	 * catalogs during promote.
	 */
	if (am_startup && (coordinator_dbid == standby_dbid))
	{
		/*
		 * Job is already done, nothing needs to be done. We mostly crashed
		 * after updating the catalogs.
		 *
		 * Logged rather than returned silently: this is the single most
		 * load-bearing line on the promotion path, and until the caller was
		 * taught to recognise a disaster-recovery replica explicitly
		 * (StartupXLOG's needToPromoteCatalog) it was also the only thing
		 * standing between a DR promotion and a deleted coordinator row.
		 */
		ereport(LOG,
				(errmsg("standby activation: dbid %d is already the coordinator in gp_segment_configuration; nothing to do",
						standby_dbid)));
		GpTopoEndWrite(ws, false);
		return true;
	}

	mirroring_sanity_check(ws, SUPERUSER | UTILITY_MODE | STANDBY_ONLY,
						   PG_FUNCNAME_MACRO);

	segment_config_activate_standby(ws, standby_dbid, coordinator_dbid);

	GpTopoCommitWrite(ws);

	/* done */
	return true;
}

Datum
gp_request_fts_probe_scan(PG_FUNCTION_ARGS)
{
	if (Gp_role != GP_ROLE_DISPATCH)
	{
		ereport(ERROR,
				(errmsg("this function can only be called by coordinator (without utility mode)")));
		PG_RETURN_BOOL(false);
	}

	FtsNotifyProber();

	PG_RETURN_BOOL(true);
}
