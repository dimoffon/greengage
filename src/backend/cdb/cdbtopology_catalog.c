/*-------------------------------------------------------------------------
 *
 * cdbtopology_catalog.c
 *	  The catalog-backed cluster-topology provider -- the default, and what
 *	  Greengage has always done.
 *
 * Topology lives in the shared catalog gp_segment_configuration.  Two read
 * paths, because a catalog read needs a transaction and one caller has none:
 * during phase 2 of 2PC the transaction is already marked committed or
 * aborted, so a RETRY_COMMIT_PREPARED needs fresh topology at a point where
 * catalog lookups are illegal.  For that, FTS keeps a flat dump of the catalog
 * in $PGDATA and the reader picks it up instead.
 *
 * That dump file is this provider's private business.  It is a cache, not an
 * authority -- readGpSegConfigFromFTSFiles() notifies FTS and waits for a
 * fresh one before reading it.  The file-backed provider is a different thing
 * entirely: there the file *is* the topology.
 *
 * Portions Copyright (c) 2005-2011, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/cdbtopology_catalog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/param.h>			/* for MAXHOSTNAMELEN */

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/gp_segment_configuration.h"
#include "catalog/indexing.h"
#include "libpq-fe.h"

#include "cdb/cdbfts.h"
#include "cdb/cdbtopology.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "miscadmin.h"
#include "postmaster/fts.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define GPSEGCONFIGDUMPFILE "gpsegconfig_dump"
#define GPSEGCONFIGDUMPFILETMP "gpsegconfig_dump_tmp"

static GpSegConfigEntry *readGpSegConfigFromCatalogRel(Relation rel, int *total_dbs);
static GpSegConfigEntry *readGpSegConfigFromCatalog(int *total_dbs);
static GpSegConfigEntry *readGpSegConfigFromFTSFiles(int *total_dbs);
static void writeGpSegConfigToFTSFiles(void);

/*
 * Helper functions for fetching latest gp_segment_configuration outside of
 * the transaction.
 *
 * In phase 2 of 2PC, current xact has been marked to TRANS_COMMIT/ABORT, 
 * COMMIT_PREPARED or ABORT_PREPARED DTM are performed, if they failed,
 * dispather disconnect and destroy all gangs and fetch the latest segment
 * configurations to do RETRY_COMMIT_PREPARED or RETRY_ABORT_PREPARED,
 * however, postgres disallow catalog lookups outside of xacts.
 *
 * readGpSegConfigFromFTSFiles() notify FTS to dump the configs from catalog
 * to a flat file and then read configurations from that file.
 */
static GpSegConfigEntry *
readGpSegConfigFromFTSFiles(int *total_dbs)
{
	FILE	*fd;
	int		idx = 0;
	int		array_size = 500;
	GpSegConfigEntry *configs = NULL;
	GpSegConfigEntry *config = NULL;

	char	hostname[MAXHOSTNAMELEN];
	char	address[MAXHOSTNAMELEN];
	char    datadir[MAXPGPATH];
	char	buf[MAXHOSTNAMELEN * 2 + MAXPGPATH + 32];

	Assert(!IsTransactionState());

	/* notify and wait FTS to finish a probe and update the dump file */
	FtsNotifyProber();	

	fd = AllocateFile(GPSEGCONFIGDUMPFILE, "r");

	if (!fd)
		elog(ERROR, "could not open gp_segment_configutation dump file:%s:%m", GPSEGCONFIGDUMPFILE);

	configs = palloc0(sizeof (GpSegConfigEntry) * array_size); 

	while (fgets(buf, sizeof(buf), fd))
	{ 
		config = &configs[idx];

		if (sscanf(buf, "%d %d %c %c %c %c %d %s %s %s", (int *)&config->dbid, (int *)&config->segindex,
				   &config->role, &config->preferred_role, &config->mode, &config->status,
				   &config->port, hostname, address, datadir) != GPSEGCONFIGNUMATTR)
		{
			FreeFile(fd);
			elog(ERROR, "invalid data in gp_segment_configuration dump file: %s:%m", GPSEGCONFIGDUMPFILE);
		}

		config->hostname = pstrdup(hostname);
		config->address = pstrdup(address);
		config->datadir = pstrdup(datadir);

		idx++;
		/*
		 * Expand CdbComponentDatabaseInfo array if we've used up
		 * currently allocated space
		 */
		if (idx >= array_size)
		{
			array_size = array_size * 2;
			configs = (GpSegConfigEntry *)
				repalloc(configs, sizeof(GpSegConfigEntry) * array_size);
		}
	}

	FreeFile(fd);

	*total_dbs = idx;
	return configs;
}

/*
 * writeGpSegConfigToFTSFiles() dump gp_segment_configuration to the file
 * GPSEGCONFIGDUMPFILE, in $PGDATA, only FTS process can use this function.
 *
 * write contents to GPSEGCONFIGDUMPFILETMP first, then rename it to
 * GPSEGCONFIGDUMPFILE, it makes lockless read and write concurrently.
 */
static void
writeGpSegConfigToFTSFiles(void)
{
	FILE	*fd;
	int		idx = 0;
	int		total_dbs = 0;
	GpSegConfigEntry *configs = NULL;
	GpSegConfigEntry *config = NULL;

	Assert(IsTransactionState());
	Assert(am_ftsprobe);

	fd = AllocateFile(GPSEGCONFIGDUMPFILETMP, "w+");

	if (!fd)
		elog(ERROR, "could not create tmp file: %s: %m", GPSEGCONFIGDUMPFILETMP);

	configs = readGpSegConfigFromCatalog(&total_dbs); 

	for (idx = 0; idx < total_dbs; idx++)
	{
		config = &configs[idx];

		if (fprintf(fd, "%d %d %c %c %c %c %d %s %s %s\n", config->dbid, config->segindex,
					config->role, config->preferred_role, config->mode, config->status,
					config->port, config->hostname, config->address, config->datadir) < 0)
		{
			FreeFile(fd);
			elog(ERROR, "could not dump gp_segment_configuration to file: %s: %m", GPSEGCONFIGDUMPFILE);
		}
	}

	FreeFile(fd);

	/* rename tmp file to permanent file */
	if (rename(GPSEGCONFIGDUMPFILETMP, GPSEGCONFIGDUMPFILE) != 0)
		elog(ERROR, "could not rename file %s to file %s: %m",
			 GPSEGCONFIGDUMPFILETMP, GPSEGCONFIGDUMPFILE);
}

/*
 * Scan a gp_segment_configuration the caller already has open.
 *
 * Split out of readGpSegConfigFromCatalog() so that a write set can read the
 * topology through the very relation whose lock it took, rather than taking a
 * second, weaker one behind its own back.
 */
static GpSegConfigEntry *
readGpSegConfigFromCatalogRel(Relation gp_seg_config_rel, int *total_dbs)
{
	int					idx = 0;
	int					array_size;
	bool				isNull;
	Datum				attr;
	HeapTuple			gp_seg_config_tuple = NULL;
	SysScanDesc			gp_seg_config_scan;
	GpSegConfigEntry	*configs;
	GpSegConfigEntry	*config;

	array_size = 500;
	configs = palloc0(sizeof(GpSegConfigEntry) * array_size);

	gp_seg_config_scan = systable_beginscan(gp_seg_config_rel, InvalidOid, false, NULL,
											0, NULL);

	while (HeapTupleIsValid(gp_seg_config_tuple = systable_getnext(gp_seg_config_scan)))
	{
		config = &configs[idx];

		/* dbid */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_dbid, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->dbid = DatumGetInt16(attr);

		/* content */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_content, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->segindex= DatumGetInt16(attr);

		/* role */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_role, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->role = DatumGetChar(attr);

		/* preferred-role */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_preferred_role, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->preferred_role = DatumGetChar(attr);

		/* mode */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_mode, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->mode = DatumGetChar(attr);

		/* status */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_status, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->status = DatumGetChar(attr);

		/* hostname */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_hostname, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->hostname = TextDatumGetCString(attr);

		/* address */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_address, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->address = TextDatumGetCString(attr);

		/* port */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_port, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->port = DatumGetInt32(attr);

		/* datadir */
		attr = heap_getattr(gp_seg_config_tuple, Anum_gp_segment_configuration_datadir, RelationGetDescr(gp_seg_config_rel), &isNull);
		Assert(!isNull);
		config->datadir = TextDatumGetCString(attr);

		idx++;

		/*
		 * Expand CdbComponentDatabaseInfo array if we've used up
		 * currently allocated space
		 */
		if (idx >= array_size)
		{
			array_size = array_size * 2;
			configs = (GpSegConfigEntry *)
				repalloc(configs, sizeof(GpSegConfigEntry) * array_size);
		}
	}

	systable_endscan(gp_seg_config_scan);

	*total_dbs = idx;
	return configs;
}

static GpSegConfigEntry *
readGpSegConfigFromCatalog(int *total_dbs)
{
	Relation			rel;
	GpSegConfigEntry	*configs;

	rel = table_open(GpSegmentConfigRelationId, AccessShareLock);
	configs = readGpSegConfigFromCatalogRel(rel, total_dbs);
	table_close(rel, AccessShareLock);

	return configs;
}


/*
 * read_all for the catalog provider.
 *
 * The transaction test is the pre-existing rule, unchanged: inside one, read
 * the catalog; outside one, read FTS's dump of it.  A generation is reported
 * for interface conformance only -- the catalog's own MVCC and the FTS version
 * stamp are what actually serialise readers and writers here, so there is
 * nothing meaningful to count.
 */
static GpSegConfigEntry *
catalog_read_all(MemoryContext cxt, int *nentries, uint64 *gen)
{
	MemoryContext oldcxt;
	GpSegConfigEntry *configs;

	oldcxt = MemoryContextSwitchTo(cxt);

	if (IsTransactionState())
		configs = readGpSegConfigFromCatalog(nentries);
	else
		configs = readGpSegConfigFromFTSFiles(nentries);

	MemoryContextSwitchTo(oldcxt);

	*gen = 0;
	return configs;
}

/*
 * begin_write for the catalog provider.
 *
 * The lock is the whole of this provider's mutex: AccessExclusiveLock for a
 * serialised writer, RowExclusiveLock for FTS, which is bit-for-bit what each
 * of them takes today.  The relation stays open in ws->provider_state so that
 * persist() writes through it and end_write() decides how to drop it.
 */
static void
catalog_begin_write(GpTopoWriteSet *ws)
{
	Relation	rel;

	/*
	 * True even in the startup process, which reaches this through
	 * UpdateCatalogForStandbyPromotion()'s own transaction.
	 */
	Assert(IsTransactionState());

	rel = table_open(GpSegmentConfigRelationId,
					 ws->level == GP_TOPO_WRITE_SERIALIZED ? AccessExclusiveLock
					 : RowExclusiveLock);
	ws->provider_state = rel;

	ws->orig = readGpSegConfigFromCatalogRel(rel, &ws->norig);

	/*
	 * No generation to compare against: what serialises two writers here is
	 * the relation lock taken just above plus the catalog's own MVCC, not a
	 * counter.  A store without either -- a file, an external key-value --
	 * needs the CAS, which is why the field exists.
	 */
	ws->gen = 0;
}

static bool
catalog_str_differs(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a != b;

	return strcmp(a, b) != 0;
}

static void
catalog_delete_dbid(Relation rel, int16 dbid)
{
	ScanKeyData scankey;
	SysScanDesc sscan;
	HeapTuple	tuple;

	ScanKeyInit(&scankey,
				Anum_gp_segment_configuration_dbid,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(dbid));

	sscan = systable_beginscan(rel, GpSegmentConfigDbidIndexId, true,
							   NULL, 1, &scankey);
	while ((tuple = systable_getnext(sscan)) != NULL)
		CatalogTupleDelete(rel, &tuple->t_self);
	systable_endscan(sscan);
}

/*
 * Update only the columns that actually changed.
 *
 * The partial mask is mandatory, not tidiness.  FTS owns role, status and
 * mode and nothing else; if an update rebuilt the whole row, every FTS probe
 * that changed a status would also rewrite hostname, address, port, datadir
 * and preferred_role from its own cycle-start snapshot -- last writer wins
 * over columns it has never owned -- and would put those rewrites in the WAL
 * that the standby coordinator and the DR replica read.
 */
static void
catalog_update_entry(Relation rel, const GpSegConfigEntry *orig,
					 const GpSegConfigEntry *work)
{
	Datum		values[Natts_gp_segment_configuration];
	bool		nulls[Natts_gp_segment_configuration];
	bool		repls[Natts_gp_segment_configuration];
	ScanKeyData scankey;
	SysScanDesc sscan;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	bool		changed = false;

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));
	MemSet(repls, false, sizeof(repls));

#define REPLACE_IF(attnum, differs, datum) \
	do { \
		if (differs) \
		{ \
			values[(attnum) - 1] = (datum); \
			repls[(attnum) - 1] = true; \
			changed = true; \
		} \
	} while (0)

	REPLACE_IF(Anum_gp_segment_configuration_content,
			   orig->segindex != work->segindex,
			   Int16GetDatum(work->segindex));
	REPLACE_IF(Anum_gp_segment_configuration_role,
			   orig->role != work->role,
			   CharGetDatum(work->role));
	REPLACE_IF(Anum_gp_segment_configuration_preferred_role,
			   orig->preferred_role != work->preferred_role,
			   CharGetDatum(work->preferred_role));
	REPLACE_IF(Anum_gp_segment_configuration_mode,
			   orig->mode != work->mode,
			   CharGetDatum(work->mode));
	REPLACE_IF(Anum_gp_segment_configuration_status,
			   orig->status != work->status,
			   CharGetDatum(work->status));
	REPLACE_IF(Anum_gp_segment_configuration_port,
			   orig->port != work->port,
			   Int32GetDatum(work->port));
	REPLACE_IF(Anum_gp_segment_configuration_hostname,
			   catalog_str_differs(orig->hostname, work->hostname),
			   CStringGetTextDatum(work->hostname));
	REPLACE_IF(Anum_gp_segment_configuration_address,
			   catalog_str_differs(orig->address, work->address),
			   CStringGetTextDatum(work->address));
	REPLACE_IF(Anum_gp_segment_configuration_datadir,
			   catalog_str_differs(orig->datadir, work->datadir),
			   CStringGetTextDatum(work->datadir));

#undef REPLACE_IF

	if (!changed)
		return;

	ScanKeyInit(&scankey,
				Anum_gp_segment_configuration_dbid,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(work->dbid));

	sscan = systable_beginscan(rel, GpSegmentConfigDbidIndexId, true,
							   NULL, 1, &scankey);

	tuple = systable_getnext(sscan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cannot find dbid=%d in %s", work->dbid,
			 RelationGetRelationName(rel));

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
								 values, nulls, repls);
	CatalogTupleUpdate(rel, &tuple->t_self, newtuple);

	systable_endscan(sscan);
	pfree(newtuple);
}

static void
catalog_insert_entry(Relation rel, const GpSegConfigEntry *work)
{
	Datum		values[Natts_gp_segment_configuration];
	bool		nulls[Natts_gp_segment_configuration];
	HeapTuple	tuple;

	MemSet(nulls, false, sizeof(nulls));

	values[Anum_gp_segment_configuration_dbid - 1] = Int16GetDatum(work->dbid);
	values[Anum_gp_segment_configuration_content - 1] = Int16GetDatum(work->segindex);
	values[Anum_gp_segment_configuration_role - 1] = CharGetDatum(work->role);
	values[Anum_gp_segment_configuration_preferred_role - 1] =
		CharGetDatum(work->preferred_role);
	values[Anum_gp_segment_configuration_mode - 1] = CharGetDatum(work->mode);
	values[Anum_gp_segment_configuration_status - 1] = CharGetDatum(work->status);
	values[Anum_gp_segment_configuration_port - 1] = Int32GetDatum(work->port);
	values[Anum_gp_segment_configuration_hostname - 1] =
		CStringGetTextDatum(work->hostname);
	values[Anum_gp_segment_configuration_address - 1] =
		CStringGetTextDatum(work->address);
	values[Anum_gp_segment_configuration_datadir - 1] =
		CStringGetTextDatum(work->datadir);

	tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tuple);
	heap_freetuple(tuple);
}

/*
 * persist for the catalog provider: turn the edited array back into the row
 * operations this catalog has always seen.
 *
 * Deletes, then updates, then inserts.  That order is load-bearing, not
 * defensive: both indexes on gp_segment_configuration are UNIQUE and not
 * deferrable, so activating a standby -- which drops the old coordinator row
 * and promotes the standby's (content, preferred_role) into the slot the old
 * row occupied -- only works if the delete is issued first.  The same holds
 * across statements for gpMgmt's remove-mirror-then-add-mirror pair.
 *
 * Known limitation, and nothing in the tree does it today: because the diff is
 * keyed on dbid, swapping (content, preferred_role) between two existing rows
 * would emit two updates that collide on index 7139 in flight.  Doing that
 * safely needs a three-phase update through a sentinel; it is not built
 * speculatively, but it should not be discovered in production either.
 */
static void
catalog_persist(GpTopoWriteSet *ws)
{
	Relation	rel = (Relation) ws->provider_state;
	int			i;

	Assert(rel != NULL);

	for (i = 0; i < ws->norig; i++)
	{
		if (GpTopoArrayFindByDbid(ws->work, ws->nwork, ws->orig[i].dbid) == NULL)
			catalog_delete_dbid(rel, ws->orig[i].dbid);
	}

	for (i = 0; i < ws->norig; i++)
	{
		GpSegConfigEntry *work;

		work = GpTopoArrayFindByDbid(ws->work, ws->nwork, ws->orig[i].dbid);
		if (work != NULL)
			catalog_update_entry(rel, &ws->orig[i], work);
	}

	for (i = 0; i < ws->nwork; i++)
	{
		if (GpTopoArrayFindByDbid(ws->orig, ws->norig, ws->work[i].dbid) == NULL)
			catalog_insert_entry(rel, &ws->work[i]);
	}

	/*
	 * Make the rows visible to the rest of this transaction.  Under this
	 * provider the statement boundary would do it anyway; it is here so the
	 * read-your-own-writes contract belongs to the interface rather than being
	 * inherited from Postgres' statement machinery.
	 */
	CommandCounterIncrement();
}

static void
catalog_end_write(GpTopoWriteSet *ws, GpTopoWriteOutcome outcome)
{
	Relation	rel = (Relation) ws->provider_state;

	if (rel == NULL)
		return;

	ws->provider_state = NULL;

	/*
	 * Transaction abort has already taken back the relcache reference and the
	 * lock; touching the relcache from an abort callback is not safe, and
	 * there is nothing left to release anyway.
	 */
	if (outcome == GP_TOPO_WRITE_ABORTED)
		return;

	/*
	 * A serialised writer keeps its AccessExclusiveLock to end of transaction,
	 * which is what every segment add and remove does today and what protects
	 * the rest of gpMgmt's multi-statement transaction.  FTS drops its
	 * RowExclusiveLock here, as it does today.
	 */
	table_close(rel, ws->level == GP_TOPO_WRITE_SERIALIZED
				? NoLock : RowExclusiveLock);
}

const GpTopologyRoutine gp_topology_catalog_routine = {
	.name = "catalog",

	.wal_logged = true,
	.allows_standby_coordinator = true,
	.readable_without_transaction = false,
	.readable_without_shmem = false,

	.startup = NULL,
	.read_all = catalog_read_all,

	.begin_write = catalog_begin_write,
	.persist = catalog_persist,
	.end_write = catalog_end_write,

	.publish_snapshot = writeGpSegConfigToFTSFiles
};
