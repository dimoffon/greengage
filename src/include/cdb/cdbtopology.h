/*-------------------------------------------------------------------------
 *
 * cdbtopology.h
 *	  Pluggable storage for the cluster topology.
 *
 * Cluster topology -- which dbids exist, what content each serves, where each
 * one lives and what state it is in -- has always lived in one place: the
 * shared catalog gp_segment_configuration.  That is a workable default and a
 * poor requirement.  Anything that has to describe a cluster *other* than the
 * one whose WAL it is replaying has to fight the catalog for it; the
 * disaster-recovery replica is the clearest case, and it paid for that fight
 * with an apply-time WAL filter, two payload-named redo guards, and a standing
 * contract that production must not VACUUM FULL its own catalogs.
 *
 * So the storage is a provider, chosen per node by the gp_topology_source GUC:
 *
 *	  catalog   the shared catalog, as always (the default; nothing changes)
 *	  file      a flat file in $PGDATA, readable with no transaction, no
 *	            catalogs, and no shared memory -- and, crucially, not
 *	            replicated, so a replica describes itself
 *
 * An external store (etcd, consul) is a third provider that is designed for
 * but not implemented here; see "Writing another provider" at the bottom of
 * this file for the contract it must satisfy.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/include/cdb/cdbtopology.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CDBTOPOLOGY_H
#define CDBTOPOLOGY_H

#include "common/gp_topology_file.h"
#include "utils/palloc.h"

/* Values of the gp_topology_source GUC. */
typedef enum GpTopologySourceKind
{
	GP_TOPOLOGY_SOURCE_CATALOG = 0,
	GP_TOPOLOGY_SOURCE_FILE
} GpTopologySourceKind;

extern int	gp_topology_source;		/* GpTopologySourceKind; int for the GUC machinery */

/*
 * A topology mutation in flight.
 *
 * Every writer reads the whole topology, edits an array, and commits.  That
 * shape is not an accident: an external store wants a compare-and-set on a
 * whole blob keyed by a revision, and a file wants a whole-file replace, so a
 * write set is what both of them are natively.  The catalog provider is the
 * odd one out and absorbs the difference by diffing `orig` against `work` on
 * the dbid key and issuing the row operations it has always issued.
 *
 * The alternative -- a row-level interface -- would push that same diff into
 * every non-catalog provider, and make each of them reproduce
 * read-your-own-writes within the transaction as well.
 */
typedef struct GpTopoWriteSet
{
	MemoryContext cxt;			/* everything below is allocated here */

	GpSegConfigEntry *orig;		/* immutable snapshot, as read */
	int			norig;
	uint64		gen;			/* store generation at snapshot time */

	GpSegConfigEntry *work;		/* mutable copy; the caller edits this */
	int			nwork;
	int			workmax;

	bool		dirty;
	void	   *provider_state; /* provider-private, e.g. an open Relation */
} GpTopoWriteSet;

typedef struct GpTopologyRoutine
{
	const char *name;

	/*
	 * Capabilities, consulted by the generic layer rather than by callers.
	 *
	 * wal_logged is the one that decides whether a provider is usable in a
	 * cluster with a standby coordinator: the standby only knows the topology
	 * because the coordinator's writes reach it, and for the catalog that
	 * happens by WAL.  A store that is not WAL-logged leaves the standby with
	 * a copy that silently goes stale, so allows_standby_coordinator tracks it.
	 */
	bool		wal_logged;
	bool		allows_standby_coordinator;
	bool		readable_without_transaction;
	bool		readable_without_shmem;

	void		(*startup) (void);

	/* Read the whole topology into `cxt`.  Never returns NULL. */
	GpSegConfigEntry *(*read_all) (MemoryContext cxt, int *nentries, uint64 *gen);

	void		(*begin_write) (GpTopoWriteSet *ws);
	void		(*persist) (GpTopoWriteSet *ws);
	void		(*end_write) (GpTopoWriteSet *ws, bool commit);
} GpTopologyRoutine;

extern const GpTopologyRoutine *GpTopoActiveProvider(void);
extern void GpTopologyProviderStartup(void);

/*
 * Read the whole topology.  Callers pass the context they want the result in:
 * getCdbComponentInfo() wants CdbComponentsContext, the SRF wants its per-call
 * context, and FTS calls this in a loop, so making the context implicit is how
 * this leaks.
 */
extern GpSegConfigEntry *GpTopologyGetAll(MemoryContext cxt, int *nentries);

/*
 * Refresh the catalog provider's out-of-transaction dump file.  FTS-only; a
 * no-op under any other provider, which do not need one.
 */
extern void writeGpSegConfigToFTSFiles(void);

/*
 * Writing another provider
 * ------------------------
 * An external store (etcd, consul) must satisfy all of:
 *
 *	1. read_all() returns a *consistent* whole-set read together with a
 *	   monotonic generation -- etcd's ModRevision, consul's ModifyIndex.
 *	2. persist() is a compare-and-set on that generation and must ERROR, never
 *	   silently overwrite, when it does not match.
 *	3. begin_write()/end_write() bracket a cluster-wide mutex held under a
 *	   lease or TTL, so a coordinator that dies mid-write cannot wedge the
 *	   store.
 *	4. If the provider is to serve the startup process, read_all() must work
 *	   with no transaction and no shared memory (set both capability bits).
 *	5. allows_standby_coordinator = true -- lifting that restriction is the
 *	   whole reason an external store exists.
 */

#endif							/* CDBTOPOLOGY_H */
