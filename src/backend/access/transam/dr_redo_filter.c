/*-------------------------------------------------------------------------
 *
 * dr_redo_filter.c
 *	  Apply-time WAL redo filter for Greengage disaster-recovery (DR) replicas.
 *
 * See dr_redo_filter.h for the rationale.  This file provides:
 *	- DRRedoShouldFilter(): the per-record predicate called from the main redo
 *	  loop in xlog.c, and
 *	- DRResolveProtectedRelfilenodes()/DRRelfilenodeIsProtected(): the protected
 *	  cluster-topology catalog set, resolved via the shared relmapper.
 *
 * The protected catalogs are shared relations, so their relfilenodes live in
 * the shared relation map (global/pg_filenode.map) rather than in pg_class.
 * We resolve OID -> filenode through RelationMapOidToFilenode(oid, shared=true)
 * and match WAL block tags in the global tablespace against the result.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/dr_redo_filter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/dr_redo_filter.h"
#include "access/xlog.h"
#include "access/xlogrecord.h"
#include "catalog/pg_tablespace_d.h"	/* GLOBALTABLESPACE_OID */
#include "utils/relmapper.h"

/*
 * The shared cluster-topology catalogs whose contents describe THIS (DR)
 * cluster, not production, and so must never be overwritten by replayed
 * production WAL: gp_segment_configuration (with its indexes and TOAST) plus
 * gp_configuration_history, gp_id and gp_version_at_initdb.
 *
 * Raw OIDs are used deliberately.  gp_version.h and gp_version_at_initdb.h both
 * define GpVersionRelationId with different values (5003 vs 5103), so pulling
 * the symbolic macros in here would be fragile; these BKI-assigned OIDs are
 * stable.
 */
static const Oid dr_protected_catalog_oids[] = {
	5036,						/* gp_segment_configuration */
	7139,						/* gp_segment_config_content_preferred_role_index */
	7140,						/* gp_segment_config_dbid_index */
	6092,						/* gp_segment_configuration TOAST table */
	6093,						/* gp_segment_configuration TOAST index */
	5106,						/* gp_configuration_history */
	5101,						/* gp_id */
	5103						/* gp_version_at_initdb */
};

#define DR_NUM_PROTECTED_CATALOGS \
	(sizeof(dr_protected_catalog_oids) / sizeof(dr_protected_catalog_oids[0]))

/* Resolved filenodes (a subset of the OIDs above that are actually mapped). */
static Oid	dr_protected_filenodes[DR_NUM_PROTECTED_CATALOGS];
static int	dr_num_protected_filenodes = 0;
static bool dr_protected_resolved = false;

/*
 * DRResolveProtectedRelfilenodes
 *		Resolve the protected topology-catalog OIDs to relfilenodes via the
 *		shared relmapper.
 *
 * Called lazily the first time the filter runs, and again (via invalidation)
 * after a shared relmap update is replayed.  Catalogs that are not mapped
 * relations are reported and left unprotected rather than silently ignored.
 */
void
DRResolveProtectedRelfilenodes(void)
{
	int			i;

	/*
	 * This runs during WAL replay, in the startup process, which has not yet
	 * loaded the shared relation map -- RelationCacheInitializePhase2() (which
	 * calls RelationMapInitializePhase2()) only runs later, in InitPostgres.
	 * Without the shared map loaded, RelationMapOidToFilenode() below returns
	 * InvalidOid for every (shared) topology catalog and the protected set would
	 * be empty, making the filter a silent no-op.  Load the shared map now; it
	 * is the on-disk map, which is exactly what redo is replaying against.
	 */
	RelationMapInitializePhase2();

	dr_num_protected_filenodes = 0;

	for (i = 0; i < DR_NUM_PROTECTED_CATALOGS; i++)
	{
		Oid			reloid = dr_protected_catalog_oids[i];
		Oid			filenode = RelationMapOidToFilenode(reloid, true /* shared */ );

		if (OidIsValid(filenode))
			dr_protected_filenodes[dr_num_protected_filenodes++] = filenode;
		else
			elog(WARNING,
				 "DR redo filter: topology catalog %u is not a mapped relation; "
				 "its pages will not be protected on this DR replica",
				 reloid);
	}

	dr_protected_resolved = true;
}

/*
 * DRInvalidateProtectedRelfilenodes
 *		Force the protected set to be recomputed on next use.
 *
 * Invoked from relmap_redo() after a shared relmap update.  Under the DR
 * contract the protected catalogs are never remapped on production (the
 * VACUUM FULL / CLUSTER / REINDEX prohibition, M1.4), so in practice the
 * recomputed set is unchanged; this only keeps us consistent if some other
 * shared catalog is remapped.
 */
void
DRInvalidateProtectedRelfilenodes(void)
{
	dr_protected_resolved = false;
}

/*
 * DRRejectForbiddenRemap
 *		Halt the DR replica if a protected topology catalog is being remapped.
 *
 * Called for each mapping in an incoming shared relmap update (see relmap_redo).
 * A protected catalog's filenode only changes via VACUUM FULL / CLUSTER /
 * REINDEX / TRUNCATE on production -- operations that would orphan this
 * replica's frozen-seeded topology rows.  Rather than silently corrupt the DR
 * topology, stop replay so an operator re-seeds this node from a fresh base
 * backup.  FATAL (not PANIC) is deliberate: it halts the postmaster instead of
 * looping crash recovery on the same record, and the caller invokes this before
 * any on-disk write so the replay LSN does not advance past the record.
 *
 * The current (pre-update) filenode is read from the live shared relmap, which
 * relmap_redo has not yet overwritten at the call site.
 */
void
DRRejectForbiddenRemap(Oid mapoid, Oid new_filenode)
{
	int			i;

	for (i = 0; i < DR_NUM_PROTECTED_CATALOGS; i++)
	{
		Oid			cur;

		if (dr_protected_catalog_oids[i] != mapoid)
			continue;

		cur = RelationMapOidToFilenode(mapoid, true /* shared */ );
		if (OidIsValid(cur) && cur != new_filenode)
			ereport(FATAL,
					(errmsg("disaster-recovery replica: protected topology catalog %u was rebuilt on production (relfilenode %u -> %u)",
							mapoid, cur, new_filenode),
					 errhint("VACUUM FULL, CLUSTER, REINDEX or TRUNCATE on a cluster-topology catalog is incompatible with an attached DR replica; re-create this DR node from a fresh base backup and re-run the topology seed.")));
		return;
	}
}

/*
 * DRRelfilenodeIsProtected
 *		Is rnode one of the protected cluster-topology catalogs?
 */
bool
DRRelfilenodeIsProtected(const RelFileNode *rnode)
{
	int			i;

	if (!dr_protected_resolved)
		return false;

	/* The protected catalogs are shared: global tablespace, no database. */
	if (rnode->spcNode != GLOBALTABLESPACE_OID || rnode->dbNode != InvalidOid)
		return false;

	for (i = 0; i < dr_num_protected_filenodes; i++)
	{
		if (rnode->relNode == dr_protected_filenodes[i])
			return true;
	}

	return false;
}

/*
 * DRRedoShouldFilter
 *		Should this WAL record's redo be skipped on a DR replica?
 *
 * Returns true only when DR mode is active and *every* block the record
 * references belongs to a protected topology catalog.  A record with no block
 * references, or one that touches any non-protected relation, is always
 * applied.  Skipping redo is safe: the replay LSN is advanced by the record
 * reader, not by the resource manager's redo routine.
 */
bool
DRRedoShouldFilter(XLogReaderState *record)
{
	bool		any_block = false;
	int			block_id;

	if (!IsDRReplicaMode())
		return false;

	if (!dr_protected_resolved)
		DRResolveProtectedRelfilenodes();

	for (block_id = 0; block_id <= XLR_MAX_BLOCK_ID; block_id++)
	{
		RelFileNode rnode;
		ForkNumber	forknum;
		BlockNumber blknum;

		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		any_block = true;
		XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blknum);

		/* Any non-protected block means we must apply the record normally. */
		if (!DRRelfilenodeIsProtected(&rnode))
			return false;
	}

	return any_block;
}
