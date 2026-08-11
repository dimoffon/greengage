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
#include "utils/memutils.h"
#include "utils/rel.h"

#define GPSEGCONFIGDUMPFILE "gpsegconfig_dump"
#define GPSEGCONFIGDUMPFILETMP "gpsegconfig_dump_tmp"

static GpSegConfigEntry *readGpSegConfigFromCatalog(int *total_dbs);
static GpSegConfigEntry *readGpSegConfigFromFTSFiles(int *total_dbs);

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
void
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

static GpSegConfigEntry *
readGpSegConfigFromCatalog(int *total_dbs)
{
	int					idx = 0;
	int					array_size;
	bool				isNull;
	Datum				attr;
	Relation			gp_seg_config_rel;
	HeapTuple			gp_seg_config_tuple = NULL;
	SysScanDesc			gp_seg_config_scan;
	GpSegConfigEntry	*configs;
	GpSegConfigEntry	*config;

	array_size = 500;
	configs = palloc0(sizeof(GpSegConfigEntry) * array_size);

	gp_seg_config_rel = table_open(GpSegmentConfigRelationId, AccessShareLock);
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

	/*
	 * We're done with the catalog config, clean them up, closing all the
	 * relations we opened.
	 */
	systable_endscan(gp_seg_config_scan);
	table_close(gp_seg_config_rel, AccessShareLock);

	*total_dbs = idx;
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

const GpTopologyRoutine gp_topology_catalog_routine = {
	.name = "catalog",

	.wal_logged = true,
	.allows_standby_coordinator = true,
	.readable_without_transaction = false,
	.readable_without_shmem = false,

	.startup = NULL,
	.read_all = catalog_read_all,

	.begin_write = NULL,
	.persist = NULL,
	.end_write = NULL
};
