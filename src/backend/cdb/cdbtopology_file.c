/*-------------------------------------------------------------------------
 *
 * cdbtopology_file.c
 *	  The file-backed cluster-topology provider.
 *
 * Topology lives in $PGDATA/gg_topology, described in
 * common/gg_topology_file.h.  What this buys, and the only reason it exists:
 * the file is not replicated, so a node that replays another cluster's WAL
 * still describes itself.  Everything the disaster-recovery replica built to
 * survive topology arriving in a replicated catalog -- an apply-time redo
 * filter, two redo guards, a single-user seed -- is a consequence of the
 * catalog being the only store.
 *
 * What it costs is written down rather than discovered:
 *
 *	- The store is not WAL-logged, so a cluster with a standby coordinator
 *	  cannot use it: the standby's copy would silently go stale.  Refused at
 *	  startup and again at gp_add_coordinator_standby().
 *	- GP_TOPO_WRITE_SERIALIZED degrades.  Its contract is whole-set exclusivity
 *	  held to end of transaction, which the catalog provider delivers by
 *	  keeping AccessExclusiveLock past end_write.  A leaf LWLock cannot.  Here
 *	  SERIALIZED means "a conflict is detected by the generation CAS at persist
 *	  time": a concurrent writer gets an error at the end rather than a block at
 *	  the beginning.
 *	- A write is durable the moment persist() renames, not when the surrounding
 *	  transaction commits.  So an abort between GpTopoCommitWrite() and
 *	  CommitTransactionCommand() leaves the topology advanced with no matching
 *	  gp_configuration_history row.  Under this provider that history is
 *	  advisory.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/cdbtopology_file.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "catalog/gp_segment_configuration_internal.h"
#include "cdb/cdbtopology.h"
#include "cdb/cdbvars.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "utils/memutils.h"

/*
 * GpTopologyLock discipline -- checkable by reading the code under it.
 *
 * 1. It is a LEAF lock.  Nothing else is acquired while it is held: no other
 *	  LWLock, no heavyweight lock, no relation, no catalog access.  The only
 *	  work under it is read the file, compare the generation, serialize, write,
 *	  fsync, rename, store the watermark.
 * 2. It is never held across a statement or transaction boundary, and never
 *	  across caller code.  It is taken and released inside file_persist().  That
 *	  is why the read-modify-write window is protected by the generation CAS and
 *	  not by this lock: that window spans arbitrary caller code -- segadmin's
 *	  precondition checks, FTS's whole message exchange.
 * 3. No provider ever takes both this and a relation lock.  The catalog
 *	  provider takes only the relation lock; this one takes only this.
 * 4. It is not taken by startup(), which runs in the postmaster, where there is
 *	  no PGPROC and an acquire that waited would PANIC.  Readers take no lock at
 *	  all: the rename is atomic, so a reader sees the whole old file or the
 *	  whole new one, and the checksum catches any residue.
 * 5. It does not exclude a writer in another process image -- a frontend
 *	  builder, single-user mode.  That exclusion is gg_topology's refusal to
 *	  write while a postmaster.pid exists.  This lock plus the CAS is what makes
 *	  a violation loud instead of silent.
 */

/*
 * Turn a format error into a report at the caller's chosen level.
 *
 * The elevel is the caller's because the same condition is a refusal to start
 * in the postmaster and a failed query in a backend.
 */
static void
file_report(int elevel, GgTopologyFileError err, int errline, const char *path)
{
	StringInfoData detail;

	initStringInfo(&detail);
	appendStringInfoString(&detail, gg_topology_file_error_str(err));
	if (errline > 0)
		appendStringInfo(&detail, " at line %d", errline);

	switch (err)
	{
		case GG_TOPOFILE_ENOENT:
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("cluster topology file \"%s\" does not exist", path),
					 errdetail("Every data directory is created with one, so it "
							   "has been removed."),
					 errhint("Recreate an empty one with \"gg_topology bootstrap "
							 "--datadir %s\", or restore this node's topology "
							 "with \"gg_topology write\".", DataDir)));
			break;

		case GG_TOPOFILE_IO:
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not read cluster topology file \"%s\": %m",
							path)));
			break;

		default:
			ereport(elevel,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("cluster topology file \"%s\" is unusable", path),
					 errdetail("%s.", detail.data)));
			break;
	}

	pfree(detail.data);
}

static void
file_topology_path(char *path, size_t pathlen)
{
	snprintf(path, pathlen, "%s/%s", DataDir, GG_TOPOLOGY_FILENAME);
}

/*
 * Read the store, reporting at `elevel` on failure.
 *
 * Returns false only when the caller's elevel does not throw, which no caller
 * currently uses; every one of them passes FATAL or ERROR.
 */
static bool
file_read(GgTopologyFile *topo, int elevel)
{
	char		path[MAXPGPATH];
	GgTopologyFileError err;
	int			errline = 0;

	file_topology_path(path, sizeof(path));

	err = gg_topology_read_file(DataDir, topo, &errline);
	if (err != GG_TOPOFILE_OK)
	{
		file_report(elevel, err, errline, path);
		return false;
	}

	return true;
}

/*
 * Warn once if the store records a different database system.
 *
 * One way only, and that asymmetry is the point: a disaster-recovery replica is
 * a physical copy of production and shares its identifier, so a match proves
 * nothing and only a mismatch carries information.  A warning rather than a
 * refusal because the case where it fires is exactly the case where the
 * operator's remaining tool is "start the node and ask it what it read".  The
 * teeth are in the writer, which refuses to overwrite a foreign file.
 */
static void
file_check_system_identifier(const GgTopologyFile *topo)
{
	uint64		mine = GetSystemIdentifier();

	if (topo->system_identifier == 0 || mine == 0)
		return;					/* not comparable */

	if (topo->system_identifier != mine)
		ereport(WARNING,
				(errmsg("cluster topology file records database system identifier "
						UINT64_FORMAT ", but this cluster is " UINT64_FORMAT,
						topo->system_identifier, mine),
				 errhint("The topology may have been copied from another cluster.")));
}

static void
file_startup(void)
{
	GgTopologyFile topo;
	char		path[MAXPGPATH];
	int			i;

	file_topology_path(path, sizeof(path));

	/*
	 * FATAL, not ERROR: this runs in the postmaster and a store it cannot read
	 * is not something to discover at the first query.  Note that Greengage
	 * escalates FATAL to PANIC when the role is dispatch and the DTX state is
	 * 2PC-critical; the postmaster does run with gp_role=dispatch, and only the
	 * never-true DTX test keeps this a FATAL.
	 */
	(void) file_read(&topo, FATAL);

	file_check_system_identifier(&topo);

	/*
	 * A standby coordinator cannot be kept in step by a store that is not
	 * WAL-logged: the standby learns the topology only because the
	 * coordinator's writes reach it, and nothing carries a file write across.
	 * Refuse here, in the postmaster, before anything can act on it.
	 */
	for (i = 0; i < topo.nentries; i++)
	{
		const GpSegConfigEntry *e = &topo.entries[i];

		if (e->segindex == COORDINATOR_CONTENT_ID &&
			e->preferred_role == GP_SEGMENT_CONFIGURATION_ROLE_MIRROR)
			ereport(FATAL,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cluster topology holds a standby coordinator, which "
							"gg_topology_source = file cannot support"),
					 errdetail("dbid %d is a standby coordinator, and the file "
							   "store is not WAL-logged, so its copy of the "
							   "topology would go stale.", e->dbid),
					 errhint("Use gg_topology_source = catalog, or an external "
							 "topology store.")));
	}

	/*
	 * generation 0 is what initdb writes and no real writer ever writes, so on
	 * a node that is meant to be dispatching it means the store was never
	 * migrated into -- an operator who set the GUC and restarted.  Serving an
	 * empty topology there would report a cluster with no segments.
	 *
	 * Gated on the role because a fresh cluster legitimately runs a
	 * utility-mode coordinator and empty segments before anything writes
	 * topology, and both must be able to start.
	 */
	if (topo.generation == 0 && Gp_role == GP_ROLE_DISPATCH)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cluster topology store has never been written"),
				 errdetail("\"%s\" is at generation 0 with %d entries, which is "
						   "the state a new data directory is created in.",
						   path, topo.nentries),
				 errhint("Export an existing topology with \"gg_topology write\", "
						 "or let gpinitsystem populate a new cluster before "
						 "starting it in dispatch mode.")));

	/*
	 * One line an operator, and ggdr's post-build check, can compare against.
	 */
	ereport(LOG,
			(errmsg("cluster topology: %d entries from \"%s\" at generation "
					UINT64_FORMAT, topo.nentries, path, topo.generation)));

	GpTopoGenerationObserve(topo.generation);

	gg_topology_file_free(&topo);
}

static GpSegConfigEntry *
file_read_all(MemoryContext cxt, int *nentries, uint64 *gen)
{
	MemoryContext oldcxt;
	GgTopologyFile topo;

	oldcxt = MemoryContextSwitchTo(cxt);

	/*
	 * No lock, in or out of a transaction.  The rename is atomic, so a reader
	 * sees one whole file or the other; the checksum catches residue.  Unlike
	 * the catalog provider, there is no separate out-of-transaction path --
	 * the file is the out-of-transaction path.
	 */
	(void) file_read(&topo, ERROR);

	/*
	 * Never NULL: read_all's contract.  An empty topology is a real answer, and
	 * getCdbComponentInfo() is what decides whether it is an acceptable one.
	 * Allocated before the switch back, so it lands in the caller's context
	 * like the rest of the result.
	 */
	if (topo.entries == NULL)
		topo.entries = palloc0(sizeof(GpSegConfigEntry));

	MemoryContextSwitchTo(oldcxt);

	*nentries = topo.nentries;
	*gen = topo.generation;

	return topo.entries;
}

static void
file_begin_write(GpTopoWriteSet *ws)
{
	GgTopologyFile topo;

	/*
	 * Deliberately without GpTopologyLock.  The window from here to persist()
	 * spans arbitrary caller code -- segadmin's checks and their ereports, an
	 * entire FTS probe exchange -- and an LWLock held across that would violate
	 * the leaf discipline, hold interrupts off throughout, and have no deadlock
	 * detection.  The generation recorded here is what makes the window safe.
	 */
	(void) file_read(&topo, ERROR);

	ws->orig = topo.entries;
	ws->norig = topo.nentries;
	ws->gen = topo.generation;
	ws->provider_state = NULL;
}

static void
file_persist(GpTopoWriteSet *ws)
{
	GgTopologyFile cur;
	GgTopologyFile next;
	char		path[MAXPGPATH];
	GgTopologyFileError err;
	int			errentry = 0;

	file_topology_path(path, sizeof(path));

	memset(&next, 0, sizeof(next));
	next.version = GG_TOPOLOGY_FORMAT_VERSION;
	next.system_identifier = GetSystemIdentifier();
	next.entries = ws->work;
	next.nentries = ws->nwork;

	LWLockAcquire(GpTopologyLock, LW_EXCLUSIVE);

	(void) file_read(&cur, ERROR);

	/*
	 * The compare-and-set.  Two writers can both snapshot generation N in
	 * begin_write, because that window is not under this lock and cannot be;
	 * the second one to arrive here sees N+1 and refuses rather than dropping
	 * the first one's work on the floor.
	 *
	 * This is also the exclusion an external store needs, so implementing it
	 * against a file is what keeps the interface honest.  What it does NOT do
	 * is exclude a writer in another process image: two processes with no lock
	 * between them can both read N and both write N+1.  That case is prevented
	 * by gg_topology refusing to write while a postmaster is running.
	 */
	if (cur.generation != ws->gen)
	{
		uint64		found = cur.generation;

		gg_topology_file_free(&cur);
		LWLockRelease(GpTopologyLock);
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("cluster topology changed underneath this transaction"),
				 errdetail("Topology is at generation " UINT64_FORMAT
						   ", expected " UINT64_FORMAT ".", found, ws->gen)));
	}

	/*
	 * And the watermark, which catches the case the CAS cannot: a store that
	 * went backwards.  An operator dropping an older copy of the file onto a
	 * running cluster leaves it consistent with itself and with any write set
	 * opened after the swap, so nothing else notices -- but this postmaster has
	 * already seen a higher generation, and writing on top of the older file
	 * would destroy whatever the difference was.
	 */
	if (cur.generation < GpTopoGenerationSeen())
	{
		uint64		found = cur.generation;
		uint64		seen = GpTopoGenerationSeen();

		gg_topology_file_free(&cur);
		LWLockRelease(GpTopologyLock);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("cluster topology store has gone backwards"),
				 errdetail("\"%s\" is at generation " UINT64_FORMAT
						   ", but generation " UINT64_FORMAT
						   " has already been served by this postmaster.",
						   path, found, seen),
				 errhint("An older copy of the file may have been restored over "
						 "the current one.")));
	}

	gg_topology_file_free(&cur);
	next.generation = ws->gen + 1;

	err = gg_topology_write_file(DataDir, &next, false, &errentry);
	if (err != GG_TOPOFILE_OK)
	{
		LWLockRelease(GpTopologyLock);

		if (err == GG_TOPOFILE_UNSERIALIZABLE && errentry < ws->nwork)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("cannot write cluster topology: dbid %d cannot be "
							"represented in \"%s\"",
							ws->work[errentry].dbid, path),
					 errdetail("A hostname, address or data directory containing "
							   "whitespace could not be read back.")));

		file_report(ERROR, err, 0, path);
	}

	GpTopoGenerationObserve(next.generation);

	LWLockRelease(GpTopologyLock);
}

static void
file_end_write(GpTopoWriteSet *ws, GpTopoWriteOutcome outcome)
{
	/*
	 * Nothing to release, in any of the three outcomes: no lock survives
	 * persist(), and nothing is open.  The write is already durable if it
	 * happened at all -- which is the divergence documented at the top of this
	 * file.
	 */
}

const GpTopologyRoutine gp_topology_file_routine = {
	.name = "file",

	.wal_logged = false,
	.allows_standby_coordinator = false,
	.readable_without_transaction = true,
	.readable_without_shmem = true,

	.startup = file_startup,
	.read_all = file_read_all,

	.begin_write = file_begin_write,
	.persist = file_persist,
	.end_write = file_end_write,

	/*
	 * The file is already the out-of-transaction copy, so there is nothing to
	 * publish.  FTS's calls become no-ops, and nothing writes gpsegconfig_dump
	 * under this provider.
	 */
	.publish_snapshot = NULL
};
