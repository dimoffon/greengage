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
 * How hard a writer needs to serialise.
 *
 * These are not interchangeable.  SERIALIZED means whole-set exclusivity held
 * to end of transaction -- AccessExclusiveLock under the catalog provider --
 * and that retention is what lets gpMgmt drive the topology through
 * intermediate states across several statements of one transaction without
 * anyone reading a half-finished cluster.  ROW is one or two rows with
 * concurrent readers unaffected, which is what an FTS probe cycle needs;
 * raising FTS to SERIALIZED would block every reader of the topology on every
 * cycle that changed anything.
 *
 * SERIALIZED is what six of the eight segment add/remove entry points already
 * took before the topology moved behind this interface.  The two exceptions
 * are gp_remove_segment() and gp_remove_coordinator_standby(), whose heaviest
 * lock was RowExclusiveLock: they are stronger now.  Neither can be given back
 * its old lock through this enum -- ROW releases at end_write, where those two
 * held to commit -- and the stronger lock is the safe direction, since both
 * read the topology and then delete from it.  Said here because "SERIALIZED is
 * what the callers already did" is true of most of them and not of those two.
 */
typedef enum GpTopoWriteLevel
{
	GP_TOPO_WRITE_ROW = 0,		/* a row or two; concurrent readers OK */
	GP_TOPO_WRITE_SERIALIZED	/* whole-set exclusivity, held to commit */
} GpTopoWriteLevel;

/*
 * How a write set ended, as the provider sees it.
 *
 * Callers only ever say commit or roll back (GpTopoEndWrite takes a bool).
 * The third case is the one a provider must not confuse with the second: on
 * transaction abort the resource manager has already taken back everything it
 * knows about -- for the catalog provider, the relation and its lock -- so
 * releasing them again is at best redundant and at worst a relcache access at
 * a point where that is not safe.  A provider holding something the
 * transaction machinery does *not* know about, such as an etcd lease or a
 * cluster-wide mutex, must release it in this case and only in this case.
 */
typedef enum GpTopoWriteOutcome
{
	GP_TOPO_WRITE_COMMIT,		/* caller finished; persist() has run */
	GP_TOPO_WRITE_ROLLBACK,		/* caller failed; transaction still live */
	GP_TOPO_WRITE_ABORTED		/* transaction aborted underneath us */
} GpTopoWriteOutcome;

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

	GpTopoWriteLevel level;
	int			nestlevel;		/* transaction nest level it was opened at */
	bool		dirty;			/* work differs from orig */
	bool		persisted;		/* at most one persist() per write set */
	void	   *provider_state; /* provider-private, e.g. an open Relation */

	struct GpTopoWriteSet *next_live;	/* cdbtopology.c's abort registry */
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

	/*
	 * Bring the store up, once, before anything can read it.  Runs in the
	 * postmaster: no PGPROC, so no lock that could wait, and no transaction.
	 * This is where a provider refuses to serve a store it cannot be correct
	 * on.
	 */
	void		(*startup) (void);

	/* Read the whole topology into `cxt`.  Never returns NULL. */
	GpSegConfigEntry *(*read_all) (MemoryContext cxt, int *nentries, uint64 *gen);

	/*
	 * Take the store's mutex at `ws->level` and fill ws->orig/norig/gen.
	 * ws->cxt is the current memory context throughout.  NULL means the
	 * provider is read-only.
	 */
	void		(*begin_write) (GpTopoWriteSet *ws);

	/*
	 * Compare-and-set on ws->gen, then make ws->work durable.  Called only
	 * when ws->dirty, and at most once per write set.
	 */
	void		(*persist) (GpTopoWriteSet *ws);
	void		(*end_write) (GpTopoWriteSet *ws, GpTopoWriteOutcome outcome);

	/*
	 * Refresh whatever out-of-transaction copy this provider keeps, if it
	 * keeps one.  NULL for providers that are already readable without a
	 * transaction, which is most of them -- the catalog is the exception.
	 */
	void		(*publish_snapshot) (void);
} GpTopologyRoutine;

extern const GpTopologyRoutine *GpTopoActiveProvider(void);
extern void GpTopologyProviderStartup(void);

/*
 * The topology generation watermark.
 *
 * A high-water mark of the store generation this postmaster has seen, for a
 * provider whose store carries one.  Zero means "unknown, go and read the
 * store" -- never "generation zero" -- which is what makes it correct after a
 * crash-and-restart, where reset_shared() re-zeroes shared memory while the
 * postmaster's own provider state survives.
 *
 * It is NOT the compare-and-set authority.  A provider that compared against
 * this instead of against the store would be blind to every write made by a
 * process that does not share this shared memory -- a frontend builder,
 * single-user mode -- which is the case the CAS exists for.  What it earns is
 * a monotonic check that the store has not gone backwards within one
 * postmaster lifetime, and a cheap number for diagnostics.
 *
 * The topology itself never goes in shared memory.
 */
extern Size GpTopologyShmemSize(void);
extern void GpTopologyShmemInit(void);
extern uint64 GpTopoGenerationSeen(void);
extern void GpTopoGenerationObserve(uint64 generation);

/*
 * Read the whole topology.  Callers pass the context they want the result in:
 * getCdbComponentInfo() wants CdbComponentsContext, the SRF wants its per-call
 * context, and FTS calls this in a loop, so making the context implicit is how
 * this leaks.
 */
extern GpSegConfigEntry *GpTopologyGetAll(MemoryContext cxt, int *nentries);

/*
 * Lookups over an array of entries, as returned by GpTopologyGetAll().  A
 * reader that never opens a write set uses these directly; the write-set verbs
 * further down are these same two applied to ws->work.
 *
 * The array is scanned linearly.  Topologies are tens of entries, and the
 * alternative -- an index probe -- is the thing that ties a caller to one
 * storage backend.
 */
extern GpSegConfigEntry *GpTopoArrayFindByDbid(GpSegConfigEntry *entries,
											   int nentries, int16 dbid);
extern GpSegConfigEntry *GpTopoArrayFindByContentRole(GpSegConfigEntry *entries,
													  int nentries, int16 content,
													  char role,
													  bool preferredNotCurrent);

/*
 * Refresh the active provider's out-of-transaction copy of the topology.
 * FTS-only, and a no-op under a provider that does not keep one.
 */
extern void GpTopoPublishSnapshot(void);

/*
 * Mutating the topology
 * ---------------------
 * Open a write set, edit ws->work, commit:
 *
 *		ws = GpTopoBeginWrite(CurTransactionContext, GP_TOPO_WRITE_SERIALIZED);
 *
 *		e = GpTopoFindByDbid(ws, dbid);
 *		if (e == NULL)
 *			elog(ERROR, ...);		 -- fine; see below
 *		e->status = GP_SEGMENT_CONFIGURATION_STATUS_DOWN;
 *		GpTopoUpdate(ws, e);
 *
 *		GpTopoCommitWrite(ws);
 *
 * ws->work is the read-your-own-writes mechanism: once the write set is open,
 * every question about the topology is answered from it, so a caller that
 * checks a precondition and then mutates does both against one snapshot taken
 * under one lock.  Nothing is written until GpTopoPersist().
 *
 * An ERROR anywhere between begin and commit needs no handling at the call
 * site: cdbtopology.c releases every write set open at the aborting
 * (sub)transaction's nest level.  What a caller must do is call
 * GpTopoEndWrite(ws, false) if it returns *normally* without committing.
 */
extern GpTopoWriteSet *GpTopoBeginWrite(MemoryContext cxt, GpTopoWriteLevel level);
extern void GpTopoPersist(GpTopoWriteSet *ws);
extern void GpTopoEndWrite(GpTopoWriteSet *ws, bool commit);
extern void GpTopoCommitWrite(GpTopoWriteSet *ws);

/*
 * Verbs over ws->work.  None of them touch storage.  The two find verbs are
 * the array lookups above, applied to the working copy.
 *
 * A caller that replaces one of the three string fields must allocate the
 * replacement in ws->cxt; the entries handed out here point into the write
 * set's own memory, which dies with it.
 */
extern GpSegConfigEntry *GpTopoFindByDbid(GpTopoWriteSet *ws, int16 dbid);
extern GpSegConfigEntry *GpTopoFindByContentRole(GpTopoWriteSet *ws, int16 content,
												 char role, bool preferredNotCurrent);
extern int16 GpTopoMaxDbid(GpTopoWriteSet *ws);
extern int16 GpTopoAvailableDbid(GpTopoWriteSet *ws);
extern int16 GpTopoNextContent(GpTopoWriteSet *ws);
extern void GpTopoInsert(GpTopoWriteSet *ws, const GpSegConfigEntry *entry);
extern void GpTopoUpdate(GpTopoWriteSet *ws, GpSegConfigEntry *entry);
extern int	GpTopoDelete(GpTopoWriteSet *ws, int16 dbid);

/*
 * Validation, in two tiers, because they have different audiences.
 *
 * KEYS is exactly what the two unique indexes on gp_segment_configuration
 * enforce.  GpTopoPersist() applies it to every write set, so under the
 * catalog provider it is redundant -- deliberately: it is how a store with no
 * indexes of its own inherits the constraint instead of reinventing it.
 *
 * CLUSTER is what a *usable* cluster additionally requires.  It is NOT called
 * from GpTopoPersist(), and wiring it in there would be a bug: gpexpand and
 * gprecoverseg legitimately drive the topology through intermediate states
 * that violate density and mirror pairing between statements of one
 * transaction.  It exists for a provider whose store is rebuilt wholesale
 * rather than edited in place, and for diagnostics.
 */
typedef enum GpTopoValidateLevel
{
	GP_TOPO_VALIDATE_KEYS = 0,
	GP_TOPO_VALIDATE_CLUSTER
} GpTopoValidateLevel;

extern void GpTopoValidate(GpTopoWriteSet *ws, GpTopoValidateLevel level);

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
