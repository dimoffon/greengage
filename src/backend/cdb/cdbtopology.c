/*-------------------------------------------------------------------------
 *
 * cdbtopology.c
 *	  Provider selection and the generic half of the cluster-topology store.
 *
 * See cdbtopology.h for what a provider is and why.  This file owns the
 * dispatch table and everything that is the same whatever the storage is; the
 * providers themselves live in cdbtopology_catalog.c and cdbtopology_file.c.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/cdbtopology.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/gp_segment_configuration.h"
#include "cdb/cdbtopology.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/shmem.h"
#include "utils/memutils.h"

int			gp_topology_source = GP_TOPOLOGY_SOURCE_CATALOG;

/*
 * The generation watermark.  See cdbtopology.h for what it is and, more
 * importantly, what it is not.
 */
typedef struct GpTopologyShmemStruct
{
	pg_atomic_uint64 generation_seen;
} GpTopologyShmemStruct;

static GpTopologyShmemStruct *GpTopologyShmem = NULL;

/*
 * Sizing runs long before the proc array exists, so this must stay a
 * compile-time constant.  In particular it must never be derived from the
 * topology: that would read the store during shared-memory sizing, turn an
 * unreadable store into a sizing failure, and have to return the same answer
 * in the postmaster and in every EXEC_BACKEND child, which a store that can
 * change between the two does not.
 */
Size
GpTopologyShmemSize(void)
{
	return MAXALIGN(sizeof(GpTopologyShmemStruct));
}

void
GpTopologyShmemInit(void)
{
	bool		found;

	GpTopologyShmem = (GpTopologyShmemStruct *)
		ShmemInitStruct("Cluster Topology", GpTopologyShmemSize(), &found);

	if (!found)
		pg_atomic_init_u64(&GpTopologyShmem->generation_seen, 0);
}

uint64
GpTopoGenerationSeen(void)
{
	if (GpTopologyShmem == NULL)
		return 0;

	return pg_atomic_read_u64(&GpTopologyShmem->generation_seen);
}

/*
 * Record that the store is at `generation`.
 *
 * Monotonic: a store that goes backwards within one postmaster lifetime -- an
 * operator dropping an old copy onto a running cluster -- leaves the watermark
 * ahead of it, which is the signal.  Callers write this under whatever lock
 * their provider uses for the store; readers need none.
 */
void
GpTopoGenerationObserve(uint64 generation)
{
	uint64		seen;

	if (GpTopologyShmem == NULL)
		return;

	seen = pg_atomic_read_u64(&GpTopologyShmem->generation_seen);
	while (generation > seen)
	{
		if (pg_atomic_compare_exchange_u64(&GpTopologyShmem->generation_seen,
										   &seen, generation))
			break;
	}
}

extern const GpTopologyRoutine gp_topology_catalog_routine;

/*
 * Indexed by GpTopologySourceKind, in the smgrsw[] style: a fixed set of
 * backends selected by an enum, no dynamic registration, no catalog involved.
 */
static const GpTopologyRoutine *const gp_topology_routines[] = {
	&gp_topology_catalog_routine	/* GP_TOPOLOGY_SOURCE_CATALOG */
};

#define NGpTopologyRoutines lengthof(gp_topology_routines)

static bool gp_topology_started = false;

const GpTopologyRoutine *
GpTopoActiveProvider(void)
{
	if (gp_topology_source < 0 || gp_topology_source >= NGpTopologyRoutines)
		elog(PANIC, "invalid gp_topology_source value %d", gp_topology_source);

	return gp_topology_routines[gp_topology_source];
}

/*
 * Initialise the active provider.
 *
 * Called eagerly from the postmaster once shared memory exists, so that an
 * unusable store is diagnosed at startup rather than at the first query, and
 * idempotently from the read and write paths, so that EXEC_BACKEND and any
 * path that reaches topology earlier than that still works.
 */
void
GpTopologyProviderStartup(void)
{
	const GpTopologyRoutine *routine;

	if (gp_topology_started)
		return;

	routine = GpTopoActiveProvider();
	if (routine->startup)
		routine->startup();

	gp_topology_started = true;
}

/*
 * Read the whole topology.
 *
 * No validation happens here on purpose.  getCdbComponentInfo() already checks
 * the structural invariants it depends on (one or two entry databases, dense
 * content ids, this node present) after building its array, and applying them
 * on the way out of the store would make every diagnostic path -- dumping the
 * topology of a broken node above all -- fail exactly when an operator needs to
 * see what it actually says.
 */
GpSegConfigEntry *
GpTopologyGetAll(MemoryContext cxt, int *nentries)
{
	const GpTopologyRoutine *routine;
	uint64		gen;

	GpTopologyProviderStartup();
	routine = GpTopoActiveProvider();

	return routine->read_all(cxt, nentries, &gen);
}

/*
 * Refresh the provider's out-of-transaction copy, if it keeps one.
 */
void
GpTopoPublishSnapshot(void)
{
	const GpTopologyRoutine *routine;

	GpTopologyProviderStartup();
	routine = GpTopoActiveProvider();

	if (routine->publish_snapshot)
		routine->publish_snapshot();
}


/* ----------------------------------------------------------------
 *						write sets
 * ----------------------------------------------------------------
 */

/*
 * Write sets that have been opened and not yet ended.
 *
 * There is normally at most one -- a write set spans a single SQL-callable
 * function or a single FTS probe pair -- but a list costs nothing and means
 * the abort path never has to reason about how many there are.
 */
static GpTopoWriteSet *gp_topo_live_writesets = NULL;
static bool gp_topo_xact_callback_registered = false;

static void
gp_topo_forget(GpTopoWriteSet *ws)
{
	GpTopoWriteSet **link = &gp_topo_live_writesets;

	while (*link != NULL)
	{
		if (*link == ws)
		{
			*link = ws->next_live;
			ws->next_live = NULL;
			return;
		}
		link = &(*link)->next_live;
	}
}

/*
 * Close a write set out, whatever the reason.
 *
 * The provider is told which of the three endings this is; see
 * GpTopoWriteOutcome.  Everything after that is generic: the write set's
 * memory goes, and the struct is left inert rather than freed, because the
 * caller allocated it and may still hold the pointer.
 */
static void
gp_topo_release(GpTopoWriteSet *ws, GpTopoWriteOutcome outcome)
{
	const GpTopologyRoutine *routine = GpTopoActiveProvider();

	if (routine->end_write)
		routine->end_write(ws, outcome);

	ws->provider_state = NULL;
	ws->orig = ws->work = NULL;
	ws->norig = ws->nwork = ws->workmax = 0;
	ws->dirty = false;

	if (ws->cxt != NULL)
	{
		MemoryContextDelete(ws->cxt);
		ws->cxt = NULL;
	}
}

/*
 * Release every write set opened at or below `nestlevel`.
 *
 * This is how an ERROR between GpTopoBeginWrite() and GpTopoCommitWrite() is
 * cleaned up, and it is deliberately the only way, so that call sites are not
 * seven copies of the same PG_TRY.  Under the catalog provider there is
 * nothing left to release by the time we get here, which is exactly why the
 * outcome is distinguished: see GpTopoWriteOutcome.
 */
static void
gp_topo_release_from(int nestlevel)
{
	GpTopoWriteSet **link = &gp_topo_live_writesets;

	while (*link != NULL)
	{
		GpTopoWriteSet *ws = *link;

		if (ws->nestlevel < nestlevel)
		{
			link = &ws->next_live;
			continue;
		}

		*link = ws->next_live;
		ws->next_live = NULL;

		gp_topo_release(ws, GP_TOPO_WRITE_ABORTED);
	}
}

static void
gp_topo_xact_callback(XactEvent event, void *arg)
{
	if (event != XACT_EVENT_ABORT && event != XACT_EVENT_PARALLEL_ABORT)
		return;

	gp_topo_release_from(0);
}

/*
 * A subtransaction rolling back is not the same event, and missing it is not
 * cosmetic: ws->cxt is a child of the aborting subtransaction's context, so a
 * write set left in the registry by, say, a gp_add_segment() inside a
 * PL/pgSQL EXCEPTION block would be a dangling pointer for the rest of the
 * transaction, and the next abort would walk it.
 */
static void
gp_topo_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						 SubTransactionId parentSubid, void *arg)
{
	if (event != SUBXACT_EVENT_ABORT_SUB)
		return;

	gp_topo_release_from(GetCurrentTransactionNestLevel());
}

static char *
topo_strdup(MemoryContext cxt, const char *str)
{
	return str != NULL ? MemoryContextStrdup(cxt, str) : NULL;
}

/*
 * Deep-copy one entry.
 *
 * hostip and hostaddrs are read-side scratch filled in by cdbutil.c's DNS
 * cache; they belong to a component array, not to the topology, and are left
 * NULL here rather than copied.
 */
static void
topo_copy_entry(MemoryContext cxt, GpSegConfigEntry *dst,
				const GpSegConfigEntry *src)
{
	memset(dst, 0, sizeof(*dst));

	dst->dbid = src->dbid;
	dst->segindex = src->segindex;
	dst->role = src->role;
	dst->preferred_role = src->preferred_role;
	dst->mode = src->mode;
	dst->status = src->status;
	dst->port = src->port;
	dst->hostname = topo_strdup(cxt, src->hostname);
	dst->address = topo_strdup(cxt, src->address);
	dst->datadir = topo_strdup(cxt, src->datadir);
}

/*
 * Open a write set: take the store's mutex, snapshot the topology, and hand
 * the caller a private copy to edit.
 *
 * `cxt` is the caller's context and holds only the GpTopoWriteSet itself; the
 * snapshot and the working copy go in a child of it, so an ERROR anywhere in
 * between is cleaned up by ordinary transaction teardown.  Every caller passes
 * a transaction-lifetime context for that reason.
 */
GpTopoWriteSet *
GpTopoBeginWrite(MemoryContext cxt, GpTopoWriteLevel level)
{
	const GpTopologyRoutine *routine;
	GpTopoWriteSet *ws;
	MemoryContext oldcxt;
	int			i;

	GpTopologyProviderStartup();
	routine = GpTopoActiveProvider();

	if (routine->begin_write == NULL)
		elog(ERROR, "topology provider \"%s\" is read-only", routine->name);

	if (!gp_topo_xact_callback_registered)
	{
		RegisterXactCallback(gp_topo_xact_callback, NULL);
		RegisterSubXactCallback(gp_topo_subxact_callback, NULL);
		gp_topo_xact_callback_registered = true;
	}

	ws = (GpTopoWriteSet *) MemoryContextAllocZero(cxt, sizeof(GpTopoWriteSet));
	ws->cxt = AllocSetContextCreate(cxt, "GpTopoWriteSet", ALLOCSET_SMALL_SIZES);
	ws->level = level;
	ws->nestlevel = GetCurrentTransactionNestLevel();

	ws->next_live = gp_topo_live_writesets;
	gp_topo_live_writesets = ws;

	oldcxt = MemoryContextSwitchTo(ws->cxt);

	routine->begin_write(ws);

	/*
	 * The working copy is deep: the caller may edit it freely without any of
	 * that reaching `orig`, which persist() diffs against.
	 */
	ws->workmax = Max(ws->norig + 4, 8);
	ws->work = (GpSegConfigEntry *) palloc0(sizeof(GpSegConfigEntry) * ws->workmax);
	for (i = 0; i < ws->norig; i++)
		topo_copy_entry(ws->cxt, &ws->work[i], &ws->orig[i]);
	ws->nwork = ws->norig;

	MemoryContextSwitchTo(oldcxt);

	return ws;
}

/*
 * Make the working copy durable.  A no-op if nothing was edited.
 */
void
GpTopoPersist(GpTopoWriteSet *ws)
{
	const GpTopologyRoutine *routine = GpTopoActiveProvider();
	MemoryContext oldcxt;

	Assert(!ws->persisted);
	ws->persisted = true;

	if (!ws->dirty)
		return;

	GpTopoValidate(ws, GP_TOPO_VALIDATE_KEYS);

	oldcxt = MemoryContextSwitchTo(ws->cxt);
	routine->persist(ws);
	MemoryContextSwitchTo(oldcxt);
}

void
GpTopoEndWrite(GpTopoWriteSet *ws, bool commit)
{
	gp_topo_forget(ws);
	gp_topo_release(ws, commit ? GP_TOPO_WRITE_COMMIT : GP_TOPO_WRITE_ROLLBACK);
}

void
GpTopoCommitWrite(GpTopoWriteSet *ws)
{
	GpTopoPersist(ws);
	GpTopoEndWrite(ws, true);
}


/* ----------------------------------------------------------------
 *				lookups over a topology array
 * ----------------------------------------------------------------
 */

GpSegConfigEntry *
GpTopoArrayFindByDbid(GpSegConfigEntry *entries, int nentries, int16 dbid)
{
	int			i;

	for (i = 0; i < nentries; i++)
	{
		if (entries[i].dbid == dbid)
			return &entries[i];
	}

	return NULL;
}

/*
 * Find the one entry serving `content` in `role`, current or preferred.
 *
 * A miss is not an error -- half of segadmin's callers test the result and
 * raise their own message.  The single-match assertion is the (content,
 * preferred_role) unique index restated for the current-role lookup, which has
 * no index behind it.
 */
GpSegConfigEntry *
GpTopoArrayFindByContentRole(GpSegConfigEntry *entries, int nentries,
							 int16 content, char role, bool preferredNotCurrent)
{
	GpSegConfigEntry *found = NULL;
	int			i;

	for (i = 0; i < nentries; i++)
	{
		GpSegConfigEntry *e = &entries[i];
		char		r = preferredNotCurrent ? e->preferred_role : e->role;

		if (e->segindex == content && r == role)
		{
			Assert(found == NULL);
			found = e;
		}
	}

	return found;
}


/* ----------------------------------------------------------------
 *			verbs over the working copy
 * ----------------------------------------------------------------
 */

GpSegConfigEntry *
GpTopoFindByDbid(GpTopoWriteSet *ws, int16 dbid)
{
	return GpTopoArrayFindByDbid(ws->work, ws->nwork, dbid);
}

GpSegConfigEntry *
GpTopoFindByContentRole(GpTopoWriteSet *ws, int16 content, char role,
						bool preferredNotCurrent)
{
	return GpTopoArrayFindByContentRole(ws->work, ws->nwork, content, role,
										preferredNotCurrent);
}

/*
 * Highest dbid in use, or 0 if the topology is empty.
 */
int16
GpTopoMaxDbid(GpTopoWriteSet *ws)
{
	int16		dbid = 0;
	int			i;

	for (i = 0; i < ws->nwork; i++)
		dbid = Max(dbid, ws->work[i].dbid);

	return dbid;
}

/*
 * Lowest dbid not in use, counting from 1.
 *
 * Deliberately the lowest free gap and not GpTopoMaxDbid() + 1: gpMgmt removes
 * a segment and adds another in one transaction and expects the freed dbid
 * back.  gp_add_coordinator_standby() is the one caller that wants max + 1.
 */
int16
GpTopoAvailableDbid(GpTopoWriteSet *ws)
{
	int32		dbid;

	for (dbid = 1;; dbid++)
	{
		if (dbid != (int16) dbid)
			elog(ERROR, "unable to find available dbid");

		if (GpTopoFindByDbid(ws, (int16) dbid) == NULL)
			return (int16) dbid;
	}
}

/*
 * The content id the next primary should take: one past the highest in use,
 * and 0 on a topology that holds no segment yet.
 *
 * The floor is the coordinator's own content, -1, which is what makes the
 * coordinator-only case come out at 0.  The predecessor of this function
 * floored at 0 instead, so Max(0, -1) was 0 and its caller's +1 handed the
 * first primary content 1 -- and gp init carried a raw
 * "UPDATE gp_segment_configuration SET content = content - 1" ever after to
 * undo it.  Folding the +1 in here is deliberate: there is exactly one
 * caller, and "highest content in use" was never the question it was asking.
 */
int16
GpTopoNextContent(GpTopoWriteSet *ws)
{
	int16		content = COORDINATOR_CONTENT_ID;
	int			i;

	for (i = 0; i < ws->nwork; i++)
		content = Max(content, ws->work[i].segindex);

	return content + 1;
}

void
GpTopoInsert(GpTopoWriteSet *ws, const GpSegConfigEntry *entry)
{
	if (ws->nwork >= ws->workmax)
	{
		int			newmax = ws->workmax * 2;

		ws->work = (GpSegConfigEntry *)
			repalloc(ws->work, sizeof(GpSegConfigEntry) * newmax);
		memset(&ws->work[ws->workmax], 0,
			   sizeof(GpSegConfigEntry) * (newmax - ws->workmax));
		ws->workmax = newmax;
	}

	topo_copy_entry(ws->cxt, &ws->work[ws->nwork], entry);
	ws->nwork++;
	ws->dirty = true;
}

/*
 * Mark an entry the caller has already edited in place.
 */
void
GpTopoUpdate(GpTopoWriteSet *ws, GpSegConfigEntry *entry)
{
	Assert(entry >= ws->work && entry < ws->work + ws->nwork);
	ws->dirty = true;
}

/*
 * Remove every entry with this dbid, and report how many there were, so a
 * caller that knows there must have been one can still say so.
 */
int
GpTopoDelete(GpTopoWriteSet *ws, int16 dbid)
{
	int			ndeleted = 0;
	int			i;

	for (i = 0; i < ws->nwork;)
	{
		if (ws->work[i].dbid != dbid)
		{
			i++;
			continue;
		}

		memmove(&ws->work[i], &ws->work[i + 1],
				sizeof(GpSegConfigEntry) * (ws->nwork - i - 1));
		ws->nwork--;
		ndeleted++;
	}

	if (ndeleted > 0)
		ws->dirty = true;

	return ndeleted;
}


/* ----------------------------------------------------------------
 *						validation
 * ----------------------------------------------------------------
 */

static const char *
topo_role_name(char role)
{
	switch (role)
	{
		case GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY:
			return "primary";
		case GP_SEGMENT_CONFIGURATION_ROLE_MIRROR:
			return "mirror";
		default:
			return "unknown";
	}
}

void
GpTopoValidate(GpTopoWriteSet *ws, GpTopoValidateLevel level)
{
	int			i,
				j;
	int			ncoordinator = 0;
	int			nstandby = 0;
	int16		maxcontent = -1;

	/* Tier 1: what the two unique indexes enforce. */
	for (i = 0; i < ws->nwork; i++)
	{
		GpSegConfigEntry *a = &ws->work[i];

		for (j = i + 1; j < ws->nwork; j++)
		{
			GpSegConfigEntry *b = &ws->work[j];

			if (a->dbid == b->dbid)
				ereport(ERROR,
						(errcode(ERRCODE_UNIQUE_VIOLATION),
						 errmsg("duplicate dbid %d in cluster topology",
								a->dbid)));

			if (a->segindex == b->segindex &&
				a->preferred_role == b->preferred_role)
				ereport(ERROR,
						(errcode(ERRCODE_UNIQUE_VIOLATION),
						 errmsg("duplicate preferred %s for content %d in cluster topology",
								topo_role_name(a->preferred_role),
								a->segindex)));
		}
	}

	if (level < GP_TOPO_VALIDATE_CLUSTER)
		return;

	/*
	 * Tier 2: what a usable cluster additionally requires.  Not reached from
	 * GpTopoPersist(); see the header for why.
	 */
	for (i = 0; i < ws->nwork; i++)
	{
		GpSegConfigEntry *e = &ws->work[i];

		if (e->dbid <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("invalid dbid %d in cluster topology", e->dbid)));

		if (e->role != GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY &&
			e->role != GP_SEGMENT_CONFIGURATION_ROLE_MIRROR)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("invalid role \"%c\" for dbid %d in cluster topology",
							e->role, e->dbid)));

		if (e->preferred_role != GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY &&
			e->preferred_role != GP_SEGMENT_CONFIGURATION_ROLE_MIRROR)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("invalid preferred role \"%c\" for dbid %d in cluster topology",
							e->preferred_role, e->dbid)));

		if (e->hostname == NULL || e->address == NULL || e->datadir == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("dbid %d in cluster topology has no hostname, address or data directory",
							e->dbid)));

		/*
		 * The flat-file formats -- both this provider's dump and the
		 * file-backed store -- are whitespace-separated, so a data directory
		 * containing a space silently truncates on the way back in.  Refuse it
		 * at the write instead.
		 */
		if (strpbrk(e->hostname, " \t\n\r") != NULL ||
			strpbrk(e->address, " \t\n\r") != NULL ||
			strpbrk(e->datadir, " \t\n\r") != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("dbid %d in cluster topology has whitespace in its hostname, address or data directory",
							e->dbid)));

		if (e->segindex == COORDINATOR_CONTENT_ID)
		{
			if (e->preferred_role == GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY)
				ncoordinator++;
			else
				nstandby++;
		}
		maxcontent = Max(maxcontent, e->segindex);
	}

	if (ncoordinator != 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("cluster topology has %d coordinators, expected exactly one",
						ncoordinator)));
	if (nstandby > 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("cluster topology has %d coordinator standbys, expected at most one",
						nstandby)));

	/* Contents must be dense from 0 up to the highest one present. */
	for (i = 0; i <= maxcontent; i++)
	{
		if (GpTopoFindByContentRole(ws, (int16) i,
									GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY,
									true) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("cluster topology has no segment for content %d", i),
					 errdetail("Contents must be dense from 0 to %d.",
							   maxcontent)));
	}
}
